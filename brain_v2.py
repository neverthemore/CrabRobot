#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Мозг Клешни 2.0 — сервер разговора на ноутбуке.

Что исправлено по сравнению со старым brain.py:
  1) Больше нет «зависания в состоянии думаю».
     - На стороне ESP32 стоит сторожевой таймер (THINK_TIMEOUT_MS).
     - Здесь ответ на вопрос гарантированно приходит: есть таймаут,
       и при недоступной нейросети отдаётся заготовка, а не пустота.
  2) Микрофон робота работает: ESP32 шлёт WAV на /robot-listen,
     здесь он распознаётся через faster-whisper и отвечает.
  3) Нет ручного ввода IP: ESP32 сам находит ноутбук по UDP.
  4) Точки входа /api/reply и /api/emotion с параметром ?e= ��
     теперь действительно приходят (раньше ESP32 слал их не туда).

Запуск:
    python brain_v2.py --host-ip 192.168.1.10
    (--esp больше не нужен, но старый параметр принимается и игнорируется)
"""
import argparse
import collections
import datetime
import ipaddress
import os
import re
import socket
import socketserver
import tempfile
import threading
import time

import requests
from flask import Flask, Response, jsonify, request

ap = argparse.ArgumentParser()
ap.add_argument("--host-ip", required=True, help="IP ноутбука в этой сети")
ap.add_argument("--esp", default="", help="устаревший параметр, игнорируется")
ap.add_argument("--port", type=int, default=8000)
ap.add_argument("--ollama", default="http://localhost:11434")
ap.add_argument("--model", default="qwen2.5:3b")
ap.add_argument("--stt", default="base", help="Whisper: base быстрее, small точнее")
ap.add_argument("--no-https", action="store_true", help="работать по HTTP без сертификата")
ap.add_argument("--no-discovery", action="store_true", help="не отвечать на UDP-автопоиск")
args = ap.parse_args()

SYSTEM = (
    "Ты краб-робот по имени Клешня. Отвечай по-русски дружелюбно и не длиннее 40 слов. "
    "Начинай ответ с одного тега: [happy], [sad] или [wow]."
)
HISTORY = collections.deque(maxlen=8)
LOCK = threading.Lock()
STT_LOCK = threading.Lock()
LLM_TIMEOUT = 25          # секунд на ответ нейросети
stt = None
stt_status = "загружаю"
esp_seen = {"ip": "", "at": 0.0}


# ------------------------------------------------------------------
#  Нейросеть
# ------------------------------------------------------------------
def ask_llm(question, remember=True):
    messages = [{"role": "system", "content": SYSTEM}] + list(HISTORY) + [
        {"role": "user", "content": question}
    ]
    response = requests.post(
        f"{args.ollama}/v1/chat/completions",
        json={
            "model": args.model,
            "messages": messages,
            "max_tokens": 80,
            "temperature": 0.4,
            "stream": False,
        },
        timeout=LLM_TIMEOUT,
    )
    response.raise_for_status()
    raw = response.json()["choices"][0]["message"]["content"].strip()
    match = re.search(r"\[(happy|sad|wow)\]", raw, re.I)
    emotion = match.group(1).lower() if match else "happy"
    answer = re.sub(r"\[(happy|sad|wow)\]", "", raw, flags=re.I).strip()
    if remember:
        HISTORY.append({"role": "user", "content": question})
        HISTORY.append({"role": "assistant", "content": raw})
    return answer[:300], emotion


def fake_answer(question):
    """Заготовка, чтобы Клешня никогда не молчала."""
    text = question.lower()
    if "зовут" in text or "имя" in text or "кто ты" in text:
        return "Меня зовут Клешня. Я краб-робот с двумя клешнями."
    if "ног" in text or "хожд" in text or "ед" in text:
        return "У меня две стороны ног на моторчиках и две клешни."
    if "дела" in text or "как ты" in text:
        return "Всё отлично, я готов работать. А у тебя?"
    if "микрофон" in text or "слыш" in text:
        return "Я слышу тебя через микрофон и отвечаю голосом на экране."
    return "Нейросеть сейчас недоступна, но я отвечаю заготовкой. Спроси ещё раз!"


def safe_llm(question):
    """Всегда возвращает ответ. Никогда не бросает исключение наружу."""
    try:
        answer, emotion = ask_llm(question)
        if not answer.strip():
            return fake_answer(question), "sad"
        return answer, emotion
    except Exception as error:
        print("Нейросеть недоступна:", error)
        return fake_answer(question), "sad"


def warmup():
    try:
        print("Прогреваю нейросеть:", safe_llm("Скажи одним словом: готов")[0])
    except Exception as error:
        print("Ollama не отвечает:", error)


# ------------------------------------------------------------------
#  Распознавание речи
# ------------------------------------------------------------------
def get_stt():
    global stt, stt_status
    with STT_LOCK:
        if stt is None:
            stt_status = "загружаю"
            try:
                print("Загружаю Whisper для голосового ввода...")
                from faster_whisper import WhisperModel
                stt = WhisperModel(args.stt, device="cpu", compute_type="int8")
                stt_status = "готов"
                print("Whisper готов к разговору")
            except Exception:
                stt_status = "ошибка"
                raise
    return stt


def warmup_stt():
    try:
        get_stt()
    except Exception as error:
        print("Whisper недоступен:", error)


def transcribe(path):
    segments, _ = get_stt().transcribe(
        path,
        language="ru",
        beam_size=1,
        vad_filter=True,
        initial_prompt="Клешня, краб-робот, привет, как дела.",
    )
    return " ".join(segment.text.strip() for segment in segments).strip()


# ------------------------------------------------------------------
#  UDP-автопоиск: ESP32 спрашивает «KLESHNY_HELLO», мы отвечаем
# ------------------------------------------------------------------
class DiscoveryHandler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            data, sock = self.request
            message = data.decode("utf-8", "ignore").strip()
            if message != "KLESHNY_HELLO":
                return
            ip = self.client_address[0]
            esp_seen["ip"] = ip
            esp_seen["at"] = time.time()
            sock.sendto(b"BRAIN_ACK", self.client_address)
            print(f"ESP32 найден: {ip}")
        except Exception as error:
            print("Ошибка автопоиска:", error)


def start_discovery():
    if args.no_discovery:
        return
    try:
        server = socketserver.ThreadingUDPServer(("", 7777), DiscoveryHandler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        print("Автопоиск ESP32 включён (UDP 7777)")
    except Exception as error:
        print("Автопоиск не запустился:", error)


# ------------------------------------------------------------------
#  Ответы роботу (ESP32 -> ноутбук -> ESP32)
# ------------------------------------------------------------------
def push_to_esp(path, body="", content_type="text/plain; charset=utf-8", timeout=2.0):
    ip = esp_seen["ip"]
    if not ip:
        return False
    try:
        response = requests.post(
            f"http://{ip}{path}",
            data=body.encode("utf-8"),
            headers={"Content-Type": content_type},
            timeout=timeout,
        )
        return response.ok
    except Exception:
        return False


def push_emotion(name):
    return push_to_esp(f"/api/emotion?e={name}")


def push_reply(text, emotion):
    return push_to_esp(f"/api/reply?e={emotion}", text)


# ------------------------------------------------------------------
#  Ядро разговора
# ------------------------------------------------------------------
def handle(question):
    started = time.perf_counter()
    push_emotion("think")
    answer, emotion = safe_llm(question)
    push_reply(answer, emotion)
    print(f"Ответ готов за {time.perf_counter() - started:.1f} с: {answer}")
    return answer, emotion


def process_audio(path):
    started = time.perf_counter()
    question = transcribe(path)
    print("Услышала:", question)
    if not question:
        answer = "Не расслышала, повтори, пожалуйста."
        push_reply(answer, "sad")
        return "", answer, "sad"
    answer, emotion = handle(question)
    print(f"Голос обработан за {time.perf_counter() - started:.1f} с")
    return question, answer, emotion


# ------------------------------------------------------------------
#  Веб-страница для телефона
# ------------------------------------------------------------------
PAGE = """<!doctype html><html lang="ru"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Говорим с Клешнёй</title>
<style>
:root{color-scheme:dark}body{max-width:680px;margin:0 auto;padding:16px;font:17px system-ui,sans-serif;background:#10131a;color:#f2f5f7}
h1{font-size:26px;margin:0 0 10px}section{margin:14px 0;padding:14px;border-radius:12px;background:#1b202b}
button,input{font:inherit}button{padding:12px 14px;margin:4px 2px;border:0;border-radius:9px;background:#3178c6;color:#fff}
button:disabled{opacity:.5}input{box-sizing:border-box;width:100%;padding:11px;border:1px solid #64748b;border-radius:8px;background:#fff;color:#111}
#voice{background:#d94b38}#voice.rec{background:#2d9d4a}#status{min-height:26px;color:#b7c2d1}
#answer{min-height:50px;white-space:pre-wrap;font-weight:600}.row{display:flex;gap:8px;align-items:center}.row button{flex:0 0 auto}
</style>
<h1>Разговор с Клешнёй</h1>
<section><div id="status">Подключение...</div><div class="row"><button id="voice" disabled>Говорить</button>
<label><input id="speak" type="checkbox" checked> Озвучивать ответ</label></div></section>
<section><b>Вы</b><p id="question">—</p><b>Клешня</b><p id="answer">—</p></section>
<section><input id="text" type="text" maxlength="240" placeholder="Напишите вопрос"><button id="send">Отправить</button></section>
<script>
const $=id=>document.getElementById(id);
let recorder=null, stopTimer=0, monitorTimer=0, audioContext=null, busy=false, voiceReady=false, guardTimer=0;
function setBusy(value){busy=value;$('send').disabled=value;$('voice').disabled=value||!voiceReady}
function show(result){
  $('question').textContent=result.q||'—';
  $('answer').textContent=result.a||'—';
  if($('speak').checked&&result.a&&'speechSynthesis'in window){
    const voice=new SpeechSynthesisUtterance(result.a);
    voice.lang='ru-RU';
    speechSynthesis.cancel();
    speechSynthesis.speak(voice);
  }
}
async function checkVoice(){
  try{
    const response=await fetch('/health',{cache:'no-store'});
    const state=await response.json();
    voiceReady=state.speech==='готов';
    $('voice').disabled=busy||!voiceReady;
    if(!busy&&!recorder){
      $('status').textContent=voiceReady?'Готова слушать.':state.speech==='ошибка'?'Whisper не загрузился. Текстовый чат работает.':'Загружаю распознавание речи. Текстовый чат уже работает.';
    }
    if(!voiceReady&&state.speech!=='ошибка')setTimeout(checkVoice,2000);
  }catch(error){
    $('status').textContent='Нет связи с сервером: '+error.message;
    setTimeout(checkVoice,2000);
  }
}
async function ask(question){
  if(busy||!question)return;
  setBusy(true);
  $('status').textContent='Клешня думает...';
  // Страховка: если сервер не ответил за 45 секунд, снимаем блокировку
  clearTimeout(guardTimer);
  guardTimer=setTimeout(()=>{setBusy(false);$('status').textContent='Долгий ответ. Попробуй ещё раз.'},45000);
  try{
    const response=await fetch('/ask',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({q:question})});
    const result=await response.json();
    if(!response.ok)throw new Error(result.a||'Ошибка');
    show(result);
    $('status').textContent='Ответ получен.';
  }catch(error){$('status').textContent='Ошибка: '+error.message}
  finally{clearTimeout(guardTimer);setBusy(false)}
}
function stopRecording(){
  if(recorder&&recorder.state!=='inactive'){$('voice').disabled=true;recorder.stop();}
}
$('voice').onclick=async()=>{
  if(recorder){stopRecording();return}
  if(!voiceReady)return;
  let stream;
  try{
    stream=await navigator.mediaDevices.getUserMedia({audio:true});
    recorder=new MediaRecorder(stream);
    const chunks=[];
    recorder.ondataavailable=event=>{if(event.data.size)chunks.push(event.data)};
    recorder.onstop=async()=>{
      clearTimeout(stopTimer);clearTimeout(monitorTimer);
      if(audioContext){audioContext.close().catch(()=>{});audioContext=null}
      stream.getTracks().forEach(track=>track.stop());
      const audio=new Blob(chunks,{type:recorder.mimeType});
      recorder=null;$('voice').textContent='Говорить';
      setBusy(true);$('status').textContent='Распознаю речь...';
      const data=new FormData();
      data.append('audio',audio,'voice.webm');
      try{
        const response=await fetch('/listen',{method:'POST',body:data});
        const result=await response.json();
        if(!response.ok)throw new Error(result.a||'Ошибка');
        show(result);$('status').textContent='Ответ получен.';
      }catch(error){$('status').textContent='Ошибка: '+error.message}
      finally{setBusy(false)}
    };
    recorder.start();
    $('send').disabled=true;
    $('voice').textContent='Закончить сейчас';
    $('status').textContent='Говорите. Запись остановится после паузы.';
    stopTimer=setTimeout(stopRecording,6000);
    const AudioContextClass=window.AudioContext||window.webkitAudioContext;
    if(AudioContextClass){
      try{
        audioContext=new AudioContextClass();
        const source=audioContext.createMediaStreamSource(stream);
        const analyser=audioContext.createAnalyser();
        analyser.fftSize=1024;
        source.connect(analyser);
        const samples=new Uint8Array(analyser.fftSize);
        let heardAt=0,silenceAt=0;
        const beganAt=performance.now();
        const detectSilence=()=>{
          if(!recorder||recorder.state==='inactive')return;
          analyser.getByteTimeDomainData(samples);
          let energy=0;
          for(const sample of samples){const value=(sample-128)/128;energy+=value*value}
          const now=performance.now();
          if(Math.sqrt(energy/samples.length)>0.018){heardAt=now;silenceAt=0}
          else if(heardAt&&!silenceAt)silenceAt=now;
          if(silenceAt&&now-silenceAt>850&&now-beganAt>1100){stopRecording();return}
          monitorTimer=setTimeout(detectSilence,120);
        };
        detectSilence();
      }catch(error){if(audioContext){audioContext.close().catch(()=>{});audioContext=null}}
    }
  }catch(error){
    clearTimeout(stopTimer);clearTimeout(monitorTimer);
    if(stream)stream.getTracks().forEach(track=>track.stop());
    recorder=null;$('voice').textContent='Говорить';
    $('send').disabled=false;$('voice').disabled=!voiceReady;
    $('status').textContent='Нет доступа к микрофону: '+error.message;
  }
};
$('send').onclick=()=>{const text=$('text').value.trim();if(!busy&&text){$('text').value='';ask(text)}};
$('text').onkeydown=event=>{if(event.key==='Enter')$('send').click()};
checkVoice();
</script></html>"""

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = 2 * 1024 * 1024


@app.get("/")
def index():
    return Response(PAGE, mimetype="text/html")


@app.get("/health")
def health():
    return jsonify(speech=stt_status, esp=esp_seen["ip"])


@app.post("/ask")
def ask():
    question = (request.get_json(silent=True) or {}).get("q", "").strip()
    if not question:
        return jsonify(q="", a="Пустой вопрос.", emo="sad"), 400
    with LOCK:
        answer, emotion = handle(question)
    return jsonify(q=question, a=answer, emo=emotion)


@app.post("/listen")
def listen():
    """Запись с микрофона телефона (multipart/form-data)."""
    audio = request.files.get("audio")
    if not audio:
        return jsonify(q="", a="Нет звука.", emo="sad"), 400
    descriptor, path = tempfile.mkstemp(suffix=".webm")
    os.close(descriptor)
    try:
        audio.save(path)
        question, answer, emotion = process_audio(path)
    except Exception as error:
        print("Ошибка распознавания:", error)
        question, answer, emotion = "", "Не смогла обработать запись.", "sad"
        push_reply(answer, "sad")
    finally:
        os.remove(path)
    return jsonify(q=question, a=answer, emo=emotion)


@app.post("/robot-listen")
def robot_listen():
    """Запись с микрофона самого робота: тело запроса — WAV (audio/wav)."""
    if request.mimetype != "audio/wav":
        return jsonify(q="", a="Нужна запись WAV.", emo="sad"), 415
    audio = request.get_data(cache=False)
    if len(audio) < 44 or audio[:4] != b"RIFF" or audio[8:12] != b"WAVE":
        return jsonify(q="", a="Повреждённая WAV-запись.", emo="sad"), 400
    descriptor, path = tempfile.mkstemp(suffix=".wav")
    os.close(descriptor)
    try:
        with open(path, "wb") as file:
            file.write(audio)
        question, answer, emotion = process_audio(path)
    except Exception as error:
        print("Ошибка записи с ESP32:", error)
        question, answer, emotion = "", "Не смогла обработать запись с микрофона.", "sad"
        push_reply(answer, "sad")
    finally:
        os.remove(path)
    return jsonify(q=question, a=answer, emo=emotion)


# ------------------------------------------------------------------
#  Точки входа, которые вызывает ESP32
# ------------------------------------------------------------------
@app.post("/api/reply")
def api_reply():
    text = request.get_data(as_text=True).strip()
    emotion = request.args.get("e", "happy")
    if not text:
        return jsonify(ok=False), 400
    print("Ответ для ESP32:", text, "->", emotion)
    return jsonify(ok=True)


@app.get("/api/emotion")
def api_emotion():
    emotion = request.args.get("e", "neutral")
    print("Эмоция для ESP32:", emotion)
    return jsonify(ok=True)


@app.get("/api/ping")
def api_ping():
    return jsonify(ok=True, name="Клешня", time=time.time())


# ------------------------------------------------------------------
#  Сертификат для HTTPS
# ------------------------------------------------------------------
def ensure_cert(ip):
    if os.path.exists("cert.pem") and os.path.exists("key.pem"):
        return
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "crab")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=60))
        .add_extension(
            x509.SubjectAlternativeName(
                [x509.IPAddress(ipaddress.ip_address(ip)), x509.DNSName("localhost")]
            ),
            critical=False,
        )
        .sign(key, hashes.SHA256())
    )
    with open("key.pem", "wb") as file:
        file.write(
            key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.TraditionalOpenSSL,
                serialization.NoEncryption(),
            )
        )
    with open("cert.pem", "wb") as file:
        file.write(cert.public_bytes(serialization.Encoding.PEM))


if __name__ == "__main__":
    start_discovery()
    threading.Thread(target=warmup, daemon=True).start()
    threading.Thread(target=warmup_stt, daemon=True).start()

    if args.no_https:
        print(f"Открой на телефоне: http://{args.host_ip}:{args.port}")
        app.run(host="0.0.0.0", port=args.port, threaded=True)
    else:
        ensure_cert(args.host_ip)
        print(f"Открой на телефоне: https://{args.host_ip}:{args.port}")
        app.run(
            host="0.0.0.0",
            port=args.port,
            ssl_context=("cert.pem", "key.pem"),
            threaded=True,
        )
