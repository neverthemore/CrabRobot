// ============================================================
//  КЛЕШНЯ 2.0 — МИКРОФОН (отдельный файл)
//
//  MAX9814 даёт аналоговый сигнал на GPIO34. Мы:
//    1) смотрим на амплитуду — шум или речь;
//    2) когда услышали речь, пишем WAV (8 кГц, 16 бит) в память;
//    3) отправляем WAV на ноутбук по HTTPS на /robot-listen;
//    4) ноутбук гоняет его через faster-whisper и отвечает.
//
//  Запись идёт в отдельной задаче на ядре 0, чтобы Wi-Fi и
//  веб-пульт не замирали, пока пишется звук.
//
//  ВАЖНО: 8 кГц, а не 16 кГц. ESP32 не успеет честно снять 16 кГц
//  через analogRead — звук будет с сильными искажениями, и Whisper
//  его не разберёт.
// ============================================================

#include "hardware.h"

static volatile bool micRecording = false;
static volatile bool micStopRequested = false;
int micLevel = 0;
int micNoiseFloor = 0;
static TaskHandle_t micTaskHandle = nullptr;
static uint32_t lastMicStartAt = 0;
static uint32_t lastMicLevelAt = 0;

// ---------- Запись WAV в буфер ----------
static void writeWavHeader(uint8_t* wav, uint32_t audioBytes) {
  memcpy(wav, "RIFF", 4);
  wav[4] = audioBytes & 0xFF; wav[5] = (audioBytes >> 8) & 0xFF;
  wav[6] = (audioBytes >> 16) & 0xFF; wav[7] = (audioBytes >> 24) & 0xFF;
  memcpy(wav + 8, "WAVEfmt ", 8);
  wav[16] = 16; wav[17] = wav[18] = wav[19] = 0;   // размер fmt-блока
  wav[20] = 1; wav[21] = 0;                        // PCM
  wav[22] = 1; wav[23] = 0;                        // один канал
  const uint32_t rate = MIC_SAMPLE_RATE;
  wav[24] = rate & 0xFF; wav[25] = (rate >> 8) & 0xFF;
  wav[26] = (rate >> 16) & 0xFF; wav[27] = (rate >> 24) & 0xFF;
  const uint32_t byteRate = rate * 2;
  wav[28] = byteRate & 0xFF; wav[29] = (byteRate >> 8) & 0xFF;
  wav[30] = (byteRate >> 16) & 0xFF; wav[31] = (byteRate >> 24) & 0xFF;
  wav[32] = 2; wav[33] = 0;                        // блок выравнивания
  wav[34] = 16; wav[35] = 0;                       // бит на сэмпл
  memcpy(wav + 36, "data", 4);
  wav[40] = audioBytes & 0xFF; wav[41] = (audioBytes >> 8) & 0xFF;
  wav[42] = (audioBytes >> 16) & 0xFF; wav[43] = (audioBytes >> 24) & 0xFF;
}

// ---------- Задача записи ----------
static void microphoneTask(void* parameter) {
  (void)parameter;
  const size_t sampleCount = (size_t)MIC_SAMPLE_RATE * MIC_MAX_SECONDS;
  const size_t length = 44 + sampleCount * 2;
  uint8_t* wav = (uint8_t*)malloc(length);
  if (!wav) {
    setStatus("Нет памяти для записи");
    micRecording = false;
    micTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  writeWavHeader(wav, 0);

  size_t written = 0;
  uint32_t nextSampleAt = micros();
  const uint32_t samplePeriodUs = 1000000UL / MIC_SAMPLE_RATE;

  uint32_t silenceSince = 0;
  bool heardSomething = false;
  const uint32_t startedAt = millis();

  while (written < sampleCount) {
    if (micStopRequested) break;

    // Пока не начали говорить — ждём, но всё равно пишем тишину
    const int raw = analogRead(PIN_MIC);
    const int level = abs(raw - 2048);
    micLevel = level;

    if (!heardSomething) {
      if (level > MIC_NOISE_GATE) {
        heardSomething = true;
        silenceSince = millis();
      } else if (millis() - startedAt > MIC_STARTUP_MS) {
        // тишина слишком долгая — запись не началась
        if (millis() - startedAt > MIC_STARTUP_MS + 1500) break;
      }
    } else {
      if (level <= MIC_NOISE_GATE) {
        if (!silenceSince) silenceSince = millis();
        else if (millis() - silenceSince > MIC_SILENCE_MS) break;
      } else {
        silenceSince = 0;
      }
    }

    // 12-битный ADC → 16-битное знаковое PCM
    int32_t pcm = ((int32_t)raw - 2048) * 16;
    if (pcm > 32767) pcm = 32767;
    if (pcm < -32768) pcm = -32768;
    wav[44 + written * 2] = pcm & 0xFF;
    wav[45 + written * 2] = (pcm >> 8) & 0xFF;
    written++;

    nextSampleAt += samplePeriodUs;
    const int32_t pause = (int32_t)(nextSampleAt - micros());
    if (pause > 0) delayMicroseconds(pause);
    else nextSampleAt = micros();
  }

  micRecording = false;
  micStopRequested = false;

  if (written < (size_t)MIC_SAMPLE_RATE * MIC_MIN_SECONDS / 2 || !heardSomething) {
    free(wav);
    setStatus("Я не расслышала");
    setTemporaryEmotion(EMO_SAD, 2200, false);
    lastMicStartAt = millis();
    micTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  // Дописываем правильный размер в заголовке
  writeWavHeader(wav, (uint32_t)(written * 2));

  setStatus("Отправляю вопрос...");
  startThinking(THINK_TIMEOUT_MS + 20000);

  int status = 0;
  String response = brainRequestBinary("/robot-listen", wav, 44 + written * 2,
                                       "audio/wav", status);
  brainConnected = (status > 0);

  free(wav);

  if (status <= 0 || !response.length()) {
    stopThinking();
    setStatus("Ноутбук не ответил");
    setTemporaryEmotion(EMO_SAD, 2200, false);
    lastMicStartAt = millis();
    micTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  // Разбор ответа: {"q":"...","a":"...","emo":"happy"}
  const String question = jsonField(response, "q");
  const String answer = jsonField(response, "a");
  const String emotionName = jsonField(response, "emo");

  stopThinking();
  if (answer.length()) {
    lastQuestion = question;
    showReplyText(answer);
    setStatus("Ответ нейросети");
    Emotion parsed = EMO_HAPPY;
    emotionFromName(emotionName, parsed);
    setTemporaryEmotion(parsed, 4200, false);
    Serial.println("Клешня услышала: " + question);
    Serial.println("Клешня: " + answer);
  } else {
    setStatus("Не расслышала, повтори");
    setTemporaryEmotion(EMO_SAD, 2200, false);
  }

  lastMicStartAt = millis();
  micTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

// ---------- Публичные функции ----------
void setupMicrophone() {
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_MIC, ADC_11db);
  micNoiseFloor = 0;
  Serial.printf("Микрофон: GPIO%d, %d Гц, до %d с\n",
                PIN_MIC, MIC_SAMPLE_RATE, MIC_MAX_SECONDS);
}

bool microphoneBusy() { return micRecording; }

int microphoneAmplitude() { return micLevel; }

void startMicrophoneRecording() {
  if (micRecording) return;
  if (millis() - lastMicStartAt < MIC_COOLDOWN_MS) {
    setStatus("Подожди немного");
    return;
  }
  if (!brainKnown()) {
    setStatus("Ноутбук не найден");
    setTemporaryEmotion(EMO_SAD, 2200, false);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setStatus("Нет Wi-Fi ноутбука");
    setTemporaryEmotion(EMO_SAD, 2200, false);
    return;
  }
  micStopRequested = false;
  micRecording = true;
  setStatus("Слушаю...");
  setEmotion(EMO_THINK, false);
  if (xTaskCreatePinnedToCore(microphoneTask, "mic", 8192, nullptr, 1, &micTaskHandle, 0) != pdPASS) {
    micRecording = false;
    setStatus("Не удалось начать запись");
  }
}

void stopMicrophoneRecording() {
  micStopRequested = true;
}

// Обновление уровня микрофона для пульта (между записями)
void updateMicLevel() {
  const uint32_t now = millis();
  if (now - lastMicLevelAt < 120) return;
  lastMicLevelAt = now;
  if (micRecording) return;

  int low = 4095;
  int high = 0;
  for (int i = 0; i < 16; i++) {
    const int sample = analogRead(PIN_MIC);
    if (sample < low) low = sample;
    if (sample > high) high = sample;
    delayMicroseconds(120);
  }
  const int level = high - low;
  micLevel = level;
  // Медленно подстраиваем порог шума
  if (level < MIC_NOISE_GATE) {
    micNoiseFloor = (micNoiseFloor * 15 + level) / 16;
  }
}
