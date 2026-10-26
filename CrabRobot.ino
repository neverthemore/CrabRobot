// ============================================================
//  КЛЕШНЯ 2.0 — основная прошивка
//
//  Что исправлено по сравнению с прошлой версией:
//  1) Клешня больше не зависает в состоянии «думаю»:
//     заведен сторожевой таймер THINK_TIMEOUT_MS — если ноутбук
//     не ответил, эмоция сама возвращается в нейтраль.
//  2) Зуммер больше не пищит постоянно:
//     пассивный зуммер звучит только через короткие «чирпы»
//     tone-сигналом, а не постоянным уровнем HIGH.
//     По умолчанию EMOTION_SOUND = false.
//  3) Микрофон MAX9814 реально записывает речь (WAV 8 кГц),
//     отправляет на ноутбук и показывает распознанный ответ.
//  4) Есть автопоиск ноутбука по UDP — не нужно вручную
//     узнавать IP ноутбука.
//
//  Порядок сборки: см. «Гайд — Клешня 2.0.md».
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiUDP.h>
#include <ESP32Servo.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <vector>
#include "hardware.h"

// ============================================================
//  СОСТОЯНИЕ
// ============================================================
Emotion emotion = EMO_NEUTRAL;
String statusText = "Запуск";
String replyText;
uint32_t replyStartedAt = 0;
uint32_t replyUntil = 0;
uint32_t emotionUntil = 0;
uint32_t thinkDeadline = 0;
bool mouthFound = false;
bool eyeFound = false;
bool brainConnected = false;
String brainHostName;
String lastQuestion;
bool movementAllowed = MOVEMENT_ENABLED;

static bool mouthDirty = true;
static bool eyeDirty = true;
static uint32_t lastEyeDrawAt = 0;
static uint32_t lastMouthDrawAt = 0;
static uint32_t lastServiceAt = 0;
static uint32_t lastDiscoveryAt = 0;
static bool stationWasConnected = false;
static bool brainWasKnown = false;

// ============================================================
//  ОБЪЕКТЫ
// ============================================================
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C mouthDisplay(U8G2_R0, U8X8_PIN_NONE);
// ВАЖНО: у программной шины U8g2 порядок аргументов — СНАЧАЛА SCL, ПОТОМ SDA:
//   U8G2_..._SW_I2C(rotation, clock, data, reset)
//                 //          SCL   SDA
U8G2_SSD1306_128X64_NONAME_F_SW_I2C eyeDisplay(U8G2_R0, PIN_EYE_SCL, PIN_EYE_SDA, U8X8_PIN_NONE);
WebServer server(80);
WiFiUDP udp;

// ============================================================
//  СЕРВОПРИВОДЫ
// ============================================================
Servo servoLeftArm;
Servo servoLeftClaw;
Servo servoRightArm;
Servo servoRightClaw;

void setupServos() {
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  servoLeftArm.setPeriodHertz(SERVO_PERIOD_HZ);
  servoLeftClaw.setPeriodHertz(SERVO_PERIOD_HZ);
  servoRightArm.setPeriodHertz(SERVO_PERIOD_HZ);
  servoRightClaw.setPeriodHertz(SERVO_PERIOD_HZ);
  servoLeftArm.attach(PIN_SRV_LARM, SERVO_MIN_US, SERVO_MAX_US);
  servoLeftClaw.attach(PIN_SRV_LCLAW, SERVO_MIN_US, SERVO_MAX_US);
  servoRightArm.attach(PIN_SRV_RARM, SERVO_MIN_US, SERVO_MAX_US);
  servoRightClaw.attach(PIN_SRV_RCLAW, SERVO_MIN_US, SERVO_MAX_US);
  homePose();
}

void setLeftArm(int angle) { servoLeftArm.write(constrain(angle, 0, 180)); }
void setRightArm(int angle) { servoRightArm.write(180 - constrain(angle, 0, 180)); }
void setLeftClaw(int angle) { servoLeftClaw.write(constrain(angle, 0, 180)); }
void setRightClaw(int angle) { servoRightClaw.write(180 - constrain(angle, 0, 180)); }

void homePose() {
  setLeftArm(HOME_LARM);
  setRightArm(HOME_RARM);
  setLeftClaw(HOME_LCLAW);
  setRightClaw(HOME_RCLAW);
}

void stopServos() {
  servoLeftArm.write(constrain(HOME_LARM, 0, 180));
  servoRightArm.write(180 - constrain(HOME_RARM, 0, 180));
  servoLeftClaw.write(constrain(HOME_LCLAW, 0, 180));
  servoRightClaw.write(180 - constrain(HOME_RCLAW, 0, 180));
}

// ============================================================
//  ЗУММЕР — пассивный, короткие чирпы
//
//  Важно: пассивный зуммер НЕ должен висеть на постоянном HIGH —
//  от этого он непрерывно гудит. Поэтому генерируем меандр
//  программно: это не занимает ШИМ-таймеры сервоприводов.
// ============================================================
static bool buzzerBusy = false;

void silenceBuzzer() {
  digitalWrite(PIN_BUZZER, LOW);
  buzzerBusy = false;
}

void beep(uint16_t durationMs) {
  if (!BUZZER_ENABLED) return;
  if (buzzerBusy) return;
  buzzerBusy = true;

  if (BUZZER_IS_ACTIVE) {
    // Активный зуммер: писк = постоянный уровень
    digitalWrite(PIN_BUZZER, HIGH);
    delay(durationMs);
    digitalWrite(PIN_BUZZER, LOW);
    buzzerBusy = false;
    return;
  }

  // Пассивный зуммер: генерируем меандр вручную.
  // Не используем tone() — он занимает тот же таймер, что и сервоприводы.
  const uint16_t halfPeriodUs = 1000000UL / BUZZER_FREQ / 2;
  const uint32_t endAt = millis() + durationMs;
  digitalWrite(PIN_BUZZER, HIGH);
  while (millis() < endAt) {
    delayMicroseconds(halfPeriodUs);
    digitalWrite(PIN_BUZZER, LOW);
    delayMicroseconds(halfPeriodUs);
    digitalWrite(PIN_BUZZER, HIGH);
  }
  digitalWrite(PIN_BUZZER, LOW);
  buzzerBusy = false;
}

void beepSequence(uint8_t count, uint16_t gapMs) {
  for (uint8_t i = 0; i < count; i++) {
    beep(BUZZER_CHIRP_MS);
    if (i + 1 < count) delay(gapMs);
  }
}

// ============================================================
//  ЭМОЦИИ И СТАТУС
// ============================================================
String emotionLabel(Emotion value) {
  switch (value) {
    case EMO_HAPPY: return "радость";
    case EMO_SAD: return "грусть";
    case EMO_WOW: return "удивление";
    case EMO_THINK: return "думаю";
    default: return "нейтрально";
  }
}

bool emotionFromName(const String& name, Emotion& out) {
  String lower = name;
  lower.toLowerCase();
  lower.trim();
  if (lower == "happy" || lower == "радость") { out = EMO_HAPPY; return true; }
  if (lower == "sad" || lower == "грусть") { out = EMO_SAD; return true; }
  if (lower == "wow" || lower == "удивление") { out = EMO_WOW; return true; }
  if (lower == "think" || lower == "думаю") { out = EMO_THINK; return true; }
  if (lower == "neutral" || lower == "нейтрально") { out = EMO_NEUTRAL; return true; }
  return false;
}

void setEmotion(Emotion next, bool withSound) {
  emotion = next;
  eyeDirty = true;
  if (!withSound || !EMOTION_SOUND) return;
  if (next == EMO_HAPPY) beepSequence(2, 60);
  else if (next == EMO_SAD) beep(260);
  else if (next == EMO_WOW) beep(110);
}

void setTemporaryEmotion(Emotion next, uint32_t durationMs, bool withSound) {
  setEmotion(next, withSound);
  emotionUntil = millis() + durationMs;
}

void setStatus(const String& text) {
  statusText = text;
  mouthDirty = true;
}

// ---- Сторожевой таймер «думаю» ----
// Раньше при потере связи с ноутбуком эмоция «думаю» оставалась
// навсегда и текст на экране переставал меняться. Теперь есть
// предельное время ожидания.
void startThinking(uint32_t timeoutMs) {
  emotion = EMO_THINK;
  eyeDirty = true;
  thinkDeadline = millis() + timeoutMs;
  setStatus("Думаю...");
}

void stopThinking() {
  thinkDeadline = 0;
  emotion = EMO_NEUTRAL;
  eyeDirty = true;
  setStatus("Нет ответа. Повтори.");
}

// ============================================================
//  ЭКРАНЫ
// ============================================================
static bool devicePresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void setupDisplays() {
  // Рот: обычная шина I2C (GPIO21/22), общая с AS5600
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  mouthDisplay.setI2CAddress(OLED_ADDRESS << 1);
  mouthDisplay.begin();
  mouthFound = devicePresent(OLED_ADDRESS);

  // Глаз: отдельная программная шина (GPIO4/5) — U8g2 сам её поднимает.
  // Проверяем через результат begin(): если экрана нет, он вернёт false.
  eyeDisplay.setI2CAddress(OLED_ADDRESS << 1);
  eyeFound = eyeDisplay.begin();
}

static std::vector<String> wrapText(U8G2& display, const String& text, int maxWidth) {
  std::vector<String> lines;
  String line;
  String word;
  String source = text + " ";
  for (unsigned int i = 0; i < source.length(); i++) {
    char symbol = source[i];
    if (symbol == ' ' || symbol == '\n') {
      if (word.length()) {
        String candidate = line.length() ? line + " " + word : word;
        if (display.getUTF8Width(candidate.c_str()) > maxWidth && line.length()) {
          lines.push_back(line);
          line = word;
        } else {
          line = candidate;
        }
        word = "";
      }
    } else if (symbol != '\r') {
      word += symbol;
    }
  }
  if (line.length()) lines.push_back(line);
  return lines;
}

static void drawEye() {
  const int x = 64;
  const int y = 31;
  bool blink = emotion == EMO_NEUTRAL && (millis() % 3800) < 160;
  eyeDisplay.clearBuffer();
  if (blink) {
    eyeDisplay.drawLine(27, y, 101, y);
  } else if (emotion == EMO_HAPPY) {
    for (int offset = 0; offset < 3; offset++)
      eyeDisplay.drawCircle(x, y + 7 + offset, 24, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  } else if (emotion == EMO_SAD) {
    eyeDisplay.drawDisc(x, y + 4, 13);
    eyeDisplay.drawLine(31, 27, 95, 13);
    eyeDisplay.drawDisc(79, 50, 3);
  } else if (emotion == EMO_WOW) {
    eyeDisplay.drawCircle(x, y, 24);
    eyeDisplay.drawCircle(x, y, 23);
    eyeDisplay.drawDisc(x, y, 8);
    eyeDisplay.drawLine(31, 8, 97, 8);
  } else if (emotion == EMO_THINK) {
    int shift = ((millis() / 250) % 3 - 1) * 5;
    eyeDisplay.drawCircle(x, y, 23);
    eyeDisplay.drawDisc(x + shift, y - 8, 8);
    eyeDisplay.drawDisc(38, 53, 3);
    eyeDisplay.drawDisc(51, 57, 3);
    eyeDisplay.drawDisc(64, 61, 3);
  } else {
    eyeDisplay.drawCircle(x, y, 23);
    eyeDisplay.drawDisc(x, y, 9);
  }
  eyeDisplay.sendBuffer();
}

static void drawMouth() {
  mouthDisplay.clearBuffer();
  mouthDisplay.setFont(u8g2_font_6x12_t_cyrillic);
  const bool hasReply = replyText.length() && millis() < replyUntil;
  if (hasReply) {
    std::vector<String> lines = wrapText(mouthDisplay, replyText, 126);
    int maximumStart = (int)lines.size() - 2;
    if (maximumStart < 0) maximumStart = 0;
    int start = (millis() - replyStartedAt) / 2200;
    if (start > maximumStart) start = maximumStart;
    for (int i = 0; i < 2 && start + i < (int)lines.size(); i++)
      mouthDisplay.drawUTF8(0, 12 + i * 15, lines[start + i].c_str());
  } else if (emotion == EMO_THINK) {
    String line = statusText.length() ? statusText : "Думаю...";
    if (mouthDisplay.getUTF8Width(line.c_str()) > 126) line = line.substring(0, 20) + "…";
    mouthDisplay.drawUTF8(0, 12, line.c_str());
    int dots = (millis() / 300) % 4;
    for (int i = 0; i < dots; i++) mouthDisplay.drawDisc(54 + i * 13, 25, 3);
  } else {
    String line = statusText.length() ? statusText : "Готов";
    if (mouthDisplay.getUTF8Width(line.c_str()) > 126) line = line.substring(0, 20) + "…";
    mouthDisplay.drawUTF8(0, 12, line.c_str());
  }
  mouthDisplay.sendBuffer();
}

void drawFace() {
  drawEye();
  drawMouth();
}

void showReplyText(const String& text) {
  replyText = text;
  replyStartedAt = millis();
  replyUntil = replyStartedAt + REPLY_MIN_MS + text.length() * REPLY_MS_PER_CHAR;
  mouthDirty = true;
}

void refreshDisplays() {
  const uint32_t now = millis();
  const bool blinkClosed = (now % 3800) < 160;
  const bool blinkChanged = emotion == EMO_NEUTRAL && blinkClosed != (lastEyeDrawAt % 3800 < 160);
  const bool thinkFrameDue = emotion == EMO_THINK && now - lastEyeDrawAt >= 250;
  if (eyeDirty || blinkChanged || thinkFrameDue) {
    drawEye();
    eyeDirty = false;
    lastEyeDrawAt = now;
  }
  const bool replyIsScrolling = replyText.length() && now < replyUntil;
  if (mouthDirty || (replyIsScrolling && now - lastMouthDrawAt >= 2200)) {
    drawMouth();
    mouthDirty = false;
    lastMouthDrawAt = now;
  }
}

// ============================================================
//  СЕТЬ И СВЯЗЬ С НОУТБУКОМ
// ============================================================
void setupWiFi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setAutoReconnect(true);
  if (strlen(STA_SSID)) {
    WiFi.begin(STA_SSID, STA_PASS);
    Serial.printf("Подключаюсь к сети: %s\n", STA_SSID);
  }
  udp.begin(DISCOVERY_PORT);
  if (strlen(BRAIN_HOST)) brainHostName = BRAIN_HOST;
}

String brainHost() { return brainHostName; }
bool brainKnown() { return brainHostName.length() > 0; }
void brainSetHost(const String& host) { brainHostName = host; }

// Универсальный запрос к ноутбуку. Возвращает тело ответа.
String brainRequest(const String& path, const String& body, const char* contentType, int& status) {
  status = 0;
  if (!brainKnown()) {
    Serial.println("Ноутбук не найден: не знаю IP");
    return "";
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Нет Wi-Fi ноутбука");
    return "";
  }
  const String scheme = BRAIN_USE_HTTPS ? "https" : "http";
  const String url = scheme + "://" + brainHostName + ":" + String(BRAIN_PORT) + path;

  HTTPClient http;
  WiFiClientSecure secure;
  WiFiClient plain;
  if (BRAIN_USE_HTTPS) {
    secure.setInsecure();
    if (!http.begin(secure, url)) return "";
  } else {
    if (!http.begin(plain, url)) return "";
  }
  http.setConnectTimeout(6000);
  http.setTimeout(30000);
  if (contentType) http.addHeader("Content-Type", contentType);
  int code = 0;
  if (body.length()) {
    code = http.POST(body.c_str());
  } else {
    code = http.GET();
  }
  String response = (code > 0) ? http.getString() : "";
  http.end();
  status = code;
  return response;
}

// Отправка бинарных данных (WAV с микрофона)
String brainRequestBinary(const String& path, uint8_t* data, size_t length,
                          const char* contentType, int& status) {
  status = 0;
  if (!brainKnown() || WiFi.status() != WL_CONNECTED) return "";
  const String scheme = BRAIN_USE_HTTPS ? "https" : "http";
  const String url = scheme + "://" + brainHostName + ":" + String(BRAIN_PORT) + path;

  HTTPClient http;
  WiFiClientSecure secure;
  WiFiClient plain;
  if (BRAIN_USE_HTTPS) {
    secure.setInsecure();
    if (!http.begin(secure, url)) return "";
  } else {
    if (!http.begin(plain, url)) return "";
  }
  http.setConnectTimeout(6000);
  http.setTimeout(90000);
  if (contentType) http.addHeader("Content-Type", contentType);
  int code = http.sendRequest("POST", data, length);
  String response = (code > 0) ? http.getString() : "";
  http.end();
  status = code;
  return response;
}

void pushEmotionToBrain(const String& emotionName) {
  if (!brainKnown() || WiFi.status() != WL_CONNECTED) return;
  int status = 0;
  brainRequest("/api/emotion?e=" + emotionName, "", nullptr, status);
}

void pushReplyToBrain(const String& text, const String& emotionName) {
  if (!brainKnown() || WiFi.status() != WL_CONNECTED) return;
  int status = 0;
  brainRequest("/api/reply?e=" + emotionName, text, "text/plain; charset=utf-8", status);
}

// --- Автопоиск ноутбука по UDP ---
// Ноутбук слушает порт 7777 и отвечает на «KLESHNY_HELLO» своим IP.
static void serviceDiscovery() {
  const uint32_t now = millis();
  if (now - lastDiscoveryAt < 3000) return;
  lastDiscoveryAt = now;
  if (WiFi.status() != WL_CONNECTED) return;

  // ответ ноутбука
  int packetSize = udp.parsePacket();
  if (packetSize > 0) {
    String message;
    message.reserve(packetSize);
    while (udp.available() > 0) message += (char)udp.read();
    message.trim();
    if (message == "BRAIN_ACK") {
      String sender = udp.remoteIP().toString();
      if (sender != brainHostName) {
        brainHostName = sender;
        Serial.printf("Ноутбук найден: %s\n", brainHostName.c_str());
      }
    }
  }

  // привет ноутбуку
  if (!brainKnown()) {
    const IPAddress broadcast = WiFi.broadcastIP();
    udp.beginPacket(broadcast, DISCOVERY_PORT);
    udp.print("KLESHNY_HELLO");
    udp.endPacket();
  }
}

// ============================================================
//  ВЕБ-ПУЛЬТ
// ============================================================
String jsonEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 12);
  for (unsigned int i = 0; i < value.length(); i++) {
    const char symbol = value[i];
    if (symbol == '"' || symbol == '\\') escaped += '\\';
    if (symbol == '\n') escaped += "\\n";
    else if (symbol == '\r') escaped += "\\r";
    else escaped += symbol;
  }
  return escaped;
}

static String stateJson() {
  String json = "{";
  json += "\"status\":\"" + jsonEscape(statusText) + "\",";
  json += "\"emotion\":\"" + emotionLabel(emotion) + "\",";
  json += "\"firmware\":\"" + String(FIRMWARE_VERSION) + "\",";
  json += "\"name\":\"" + String(ROBOT_NAME) + "\",";
  json += "\"micLevel\":" + String(micLevel) + ",";
  json += "\"micNoise\":" + String(micNoiseFloor) + ",";
  json += "\"recording\":" + String(microphoneBusy() ? "true" : "false") + ",";
  json += "\"mouthFound\":" + String(mouthFound ? "true" : "false") + ",";
  json += "\"eyeFound\":" + String(eyeFound ? "true" : "false") + ",";
  json += "\"movement\":" + String(movementAllowed ? "true" : "false") + ",";
  json += "\"brain\":\"" + jsonEscape(brainHostName) + "\",";
  json += "\"brainOnline\":" + String(brainConnected ? "true" : "false") + ",";
  json += "\"question\":\"" + jsonEscape(lastQuestion) + "\",";
  json += "\"reply\":\"" + jsonEscape(replyText) + "\"";
  json += "}";
  return json;
}

static void sendState(int statusCode = 200) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(statusCode, "application/json; charset=utf-8", stateJson());
}

// Простейший разбор JSON-поля вида "ключ":"значение"
String jsonField(const String& json, const String& key) {
  String needle = "\"" + key + "\":";
  int at = json.indexOf(needle);
  if (at < 0) return "";
  at += needle.length();
  while (at < (int)json.length() && json[at] == ' ') at++;
  if (at >= (int)json.length()) return "";
  if (json[at] == '"') {
    at++;
    String value;
    while (at < (int)json.length() && json[at] != '"') {
      if (json[at] == '\\' && at + 1 < (int)json.length()) {
        at++;
        if (json[at] == 'n') value += '\n';
        else if (json[at] == 't') value += '\t';
        else value += json[at];
      } else {
        value += json[at];
      }
      at++;
    }
    return value;
  }
  String value;
  while (at < (int)json.length() && json[at] != ',' && json[at] != '}') {
    value += json[at];
    at++;
  }
  value.trim();
  return value;
}

static void handleApiReply() {
  const String text = server.arg("plain");
  const String emotionName = server.arg("e");
  if (!text.length()) {
    sendState(400);
    return;
  }
  stopThinking();
  showReplyText(text);
  setStatus("Ответ нейросети");
  Emotion parsed = EMO_HAPPY;
  emotionFromName(emotionName, parsed);
  setTemporaryEmotion(parsed, 4200, false);
  Serial.println("Ответ с ноутбука: " + text);
  sendState();
}

static void handleApiEmotion() {
  const String name = server.arg("e");
  Emotion parsed = EMO_NEUTRAL;
  if (!emotionFromName(name, parsed)) {
    sendState(400);
    return;
  }
  stopThinking();
  setTemporaryEmotion(parsed, EMOTION_SOUND_MS, false);
  setStatus("Эмоция: " + emotionLabel(parsed));
  sendState();
}

// Отправить вопрос ноутбуку и дождаться ответа
static void askBrain(const String& question) {
  if (!question.length()) return;
  lastQuestion = question;
  startThinking(THINK_TIMEOUT_MS);
  setStatus("Думаю...");

  int status = 0;
  const String body = "{\"q\":\"" + jsonEscape(question) + "\"}";
  const String response = brainRequest("/ask", body, "application/json", status);
  brainConnected = (status > 0);

  if (status <= 0 || !response.length()) {
    stopThinking();
    setStatus("Ноутбук не ответил");
    return;
  }
  const String answer = jsonField(response, "a");
  const String emotionName = jsonField(response, "emo");
  if (!answer.length()) {
    stopThinking();
    setStatus("Пустой ответ");
    return;
  }
  stopThinking();
  showReplyText(answer);
  setStatus("Ответ нейросети");
  Emotion parsed = EMO_HAPPY;
  emotionFromName(emotionName, parsed);
  setTemporaryEmotion(parsed, 4200, false);
  Serial.println("Клешня: " + answer);
}

static void handleApiQuestion() {
  String question = server.arg("plain");
  question.trim();
  if (!question.length()) {
    sendState(400);
    return;
  }
  askBrain(question);
  sendState();
}

static void handleApiMic() {
  startMicrophoneRecording();
  sendState();
}

static void handleApiBeep() {
  beepSequence(2, 70);
  setStatus("Зуммер работает");
  sendState();
}

static void handleApiStop() {
  motorsStop();
  stopMicrophoneRecording();
  stopThinking();
  setStatus("Остановлено");
  sendState();
}

// ---- Ходьба ----
static void driveFor(MotorCommand command, int speedPercent, int durationMs) {
  if (!movementEnabled()) {
    setStatus("Ходьба выключена");
    sendState();
    return;
  }
  switch (command) {
    case MOTOR_FORWARD: driveForward(speedPercent); break;
    case MOTOR_BACKWARD: driveBackward(speedPercent); break;
    case MOTOR_STRAFE_LEFT: driveStrafeLeft(speedPercent); break;
    case MOTOR_STRAFE_RIGHT: driveStrafeRight(speedPercent); break;
    case MOTOR_STOP: motorsStop(); setStatus("Готов"); sendState(); return;
  }
  // едем заданное время, потом стоп
  const uint32_t endAt = millis() + durationMs;
  while (millis() < endAt) {
    server.handleClient();     // не теряем связь с пультом во время езды
    delay(10);
  }
  motorsStop();
  setStatus("Готов");
  sendState();
}

static const char PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html lang="ru"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Клешня — пульт</title><style>
:root{color-scheme:dark}body{max-width:680px;margin:0 auto;padding:16px;font:17px system-ui,sans-serif;background:#10131a;color:#f2f5f7}
h1{font-size:24px;margin:0 0 10px}section{margin:14px 0;padding:14px;border-radius:12px;background:#1b202b}
button,input{font:inherit}button{padding:11px 13px;margin:4px 2px;border:0;border-radius:9px;background:#3178c6;color:#fff}
input[type=text]{box-sizing:border-box;width:100%;padding:10px;margin:6px 0;border:1px solid #64748b;border-radius:8px;background:#fff;color:#111}
.muted{color:#b7c2d1}.good{color:#7ee787}.warn{color:#ffd166}
</style>
<h1>Пульт Клешни</h1>
<section><b id="status">Подключение...</b><p id="hw" class="muted"></p><p id="brain" class="muted"></p></section>
<section><b>Эмоции</b><p>
<button data-e="neutral">Нейтрально</button><button data-e="happy">Радость</button>
<button data-e="sad">Грусть</button><button data-e="wow">Удивление</button><button data-e="think">Думаю</button>
</p><button id="beep">Проверить зуммер</button></section>
<section><b>Спросить</b><input id="q" type="text" maxlength="240" placeholder="Например: кто ты?"><button id="send">Спросить</button>
<button id="mic">Сказать Клешне</button><p id="answer" class="muted">Здесь появится ответ.</p></section>
<section id="moveSection" style="display:none"><b>Ходьба</b><p>
<button data-m="forward">Вперёд</button><button data-m="backward">Назад</button>
<button data-m="left">Боком влево</button><button data-m="right">Боком вправо</button>
</p><label>Скорость: <span id="speedValue">50</span> <input id="speed" type="range" min="20" max="100" value="50"></label>
<label>Время, мс: <span id="msValue">1000</span> <input id="ms" type="range" min="200" max="4000" step="100" value="1000"></label>
<button data-m="stop" style="background:#c0392b">СТОП</button>
<p><b>Проверка по одному мотору</b> (подними робота со стола!)</p>
<button data-t="a">Мотор А вперёд</button><button data-t="ar">Мотор А назад</button>
<button data-t="b">Мотор Б вперёд</button><button data-t="br">Мотор Б назад</button>
<p><b>Проверка пина</b> (мультиметр на контакт пина и GND)</p>
<p id="pinRow"></p></section>
<script>
const $=id=>document.getElementById(id);
let busy=false, refreshing=false;
async function api(path,options){const r=await fetch(path,options);const d=await r.json();if(!r.ok)throw new Error(d.status||'Ошибка');return d}
function render(s){
  $('status').textContent=s.status+' — '+s.emotion;
  $('hw').textContent=(s.mouthFound?'Рот найден. ':'Рот НЕ найден. ')+(s.eyeFound?'Глаз найден.':'Глаз НЕ найден. ')+'Прошивка '+s.firmware+(s.movement?' · ходьба включена':' · ходьба выключена');
  $('brain').textContent='Ноутбук: '+(s.brain?(s.brain+' '+(s.brainOnline?'(на связи)':'(не отвечает)')):'не найден, жду...');
  $('status').className=s.emotion==='думаю'?'warn':'';
  $('moveSection').style.display=s.movement?'block':'none';
  // Кнопки проверки пинов
  if(!$('pinRow').dataset.built){
    const pins=[23,32,14,16,18,19,13,25,27,26,17,33];
    $('pinRow').innerHTML=pins.map(p=>'<button data-p="'+p+'">GPIO'+p+'</button>').join('');
    $('pinRow').querySelectorAll('[data-p]').forEach(b=>b.onclick=async()=>{
      $('status').textContent='Проверяю GPIO'+b.dataset.p+'...';
      try{
        const body=new URLSearchParams({pin:b.dataset.p,level:'1',ms:'2000'});
        await fetch('/api/pintest',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});
      }catch(e){$('status').textContent='Ошибка: '+e.message}
    });
    $('pinRow').dataset.built='1';
  }
}
async function refresh(){if(refreshing)return;refreshing=true;try{render(await api('/api/state',{cache:'no-store'}))}catch(e){$('status').textContent='Нет связи с Клешней'}finally{refreshing=false;setTimeout(refresh,1200)}}
$('send').onclick=async()=>{const t=$('q').value.trim();if(!t||busy)return;busy=true;$('send').disabled=true;$('answer').textContent='Клешня думает...';try{const s=await api('/api/question',{method:'POST',headers:{'Content-Type':'text/plain; charset=utf-8'},body:t});render(s);$('answer').textContent=s.reply||'(пусто)';$('q').value=''}catch(e){$('answer').textContent='Ошибка: '+e.message}finally{busy=false;$('send').disabled=false}};
$('q').onkeydown=e=>{if(e.key==='Enter')$('send').click()};
$('mic').onclick=async()=>{try{const s=await api('/api/mic',{method:'POST'});render(s);$('answer').textContent='Записываю...'}catch(e){$('answer').textContent='Ошибка: '+e.message}};
$('beep').onclick=async()=>{try{render(await api('/api/beep'))}catch(e){$('status').textContent=e.message}};
document.querySelectorAll('[data-e]').forEach(b=>b.onclick=async()=>{try{render(await api('/api/emotion?e='+b.dataset.e))}catch(e){$('status').textContent=e.message}});
document.querySelectorAll('[data-m]').forEach(b=>b.onclick=async()=>{
  const cmd=b.dataset.m;
  if(cmd==='stop'){try{render(await api('/api/stop',{method:'POST'}))}catch(e){$('status').textContent=e.message}return}
  $('status').textContent='Еду...';
  try{
    const body=new URLSearchParams({cmd:cmd,speed:String($('speed').value),ms:String($('ms').value)});
    const r=await fetch('/api/move',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});
    render(await r.json());
  }catch(e){$('status').textContent='Ошибка: '+e.message}
});
$('speed').oninput=e=>$('speedValue').textContent=e.target.value;
$('ms').oninput=e=>$('msValue').textContent=e.target.value;
document.querySelectorAll('[data-t]').forEach(b=>b.onclick=async()=>{
  const t=b.dataset.t;
  const motor=(t==='a'||t==='ar')?0:1;
  const forward=(t==='a'||t==='b');
  const speed=Number($('speed').value);
  const ms=Number($('ms').value);
  $('status').textContent='Проверка мотора...';
  try{
    const body=new URLSearchParams({motor:String(motor),fwd:forward?'1':'0',speed:String(speed),ms:String(ms)});
    const r=await fetch('/api/motor',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});
    render(await r.json());
  }catch(e){$('status').textContent='Ошибка: '+e.message}
});
refresh();
</script></html>
)rawliteral";

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  setupDisplays();
  setupServos();
  setupMicrophone();
  setupMotors();

  setupWiFi();

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", PAGE);
  });
  server.on("/api/state", HTTP_GET, []() { sendState(); });
  server.on("/api/reply", HTTP_POST, handleApiReply);
  server.on("/api/emotion", HTTP_GET, handleApiEmotion);
  server.on("/api/question", HTTP_POST, handleApiQuestion);
  server.on("/api/mic", HTTP_POST, handleApiMic);
  server.on("/api/beep", HTTP_GET, handleApiBeep);
  server.on("/api/stop", HTTP_POST, handleApiStop);
  server.on("/api/motor", HTTP_POST, []() {
    const int motor = constrain(server.arg("motor").toInt(), 0, 1);
    const bool forward = server.arg("fwd") == "1";
    const int speed = constrain(server.arg("speed").toInt(), 0, 100);
    const int duration = constrain(server.arg("ms").toInt(), 100, 10000);
    if (!movementEnabled()) {
      setStatus("Ходьба выключена");
      sendState(409);
      return;
    }
    setStatus(motor == 0 ? "Мотор А" : "Мотор Б");
    driveOneMotor(motor, forward, speed);
    const uint32_t endAt = millis() + duration;
    while (millis() < endAt) {
      server.handleClient();
      delay(10);
    }
    motorsStop();
    setStatus("Готов");
    sendState();
  });
  server.on("/api/pintest", HTTP_POST, []() {
    // Ручная проверка: ставим HIGH на выбранный пин на 2 секунды.
    // Мультиметром проверь, есть ли напряжение — так ищем неверный провод.
    const int pin = server.arg("pin").toInt();
    const int level = server.arg("level").toInt() ? 1 : 0;
    const int duration = constrain(server.arg("ms").toInt(), 200, 10000);
    const uint8_t pins[] = {
      PIN_AIN1, PIN_AIN2, PIN_BIN1, PIN_BIN2, PIN_STBY,
      PIN_BUZZER, PIN_SRV_LARM, PIN_SRV_LCLAW, PIN_SRV_RARM, PIN_SRV_RCLAW,
    };
    bool known = false;
    for (uint8_t candidate : pins) {
      if ((int)candidate == pin) known = true;
    }
    if (PIN_PWMA >= 0 && pin == PIN_PWMA) known = true;
    if (PIN_PWMB >= 0 && pin == PIN_PWMB) known = true;
    if (!known) {
      setStatus("Такой пин не проверяем");
      sendState(400);
      return;
    }
    // Пин под LEDC нельзя дёргать digitalWrite — только если он не моторный.
    const bool isMotorPin = (pin == PIN_AIN1 || pin == PIN_AIN2 ||
                              pin == PIN_BIN1 || pin == PIN_BIN2 ||
                              (PIN_PWMA >= 0 && pin == PIN_PWMA) ||
                              (PIN_PWMB >= 0 && pin == PIN_PWMB));
    if (isMotorPin) {
      // Для моторных пинов уровень задаём через ШИМ
      const int duty = level ? 255 : 0;
      ledcWrite((uint8_t)pin, duty);
    } else {
      pinMode((uint8_t)pin, OUTPUT);
      digitalWrite((uint8_t)pin, level ? HIGH : LOW);
    }
    setStatus(String("Пин ") + pin + " = " + (level ? "HIGH" : "LOW"));
    sendState();
    const uint32_t endAt = millis() + duration;
    while (millis() < endAt) {
      server.handleClient();
      delay(10);
    }
    if (isMotorPin) {
      ledcWrite((uint8_t)pin, 0);
    } else {
      digitalWrite((uint8_t)pin, LOW);
    }
    setStatus("Готов");
    sendState();
  });
  server.on("/api/move", HTTP_POST, []() {
    const String command = server.arg("cmd");
    const int speed = constrain(server.arg("speed").toInt(), 0, 100);
    const int duration = constrain(server.arg("ms").toInt(), 0, 10000);
    if (command == "forward") driveFor(MOTOR_FORWARD, speed, duration);
    else if (command == "backward") driveFor(MOTOR_BACKWARD, speed, duration);
    else if (command == "left") driveFor(MOTOR_STRAFE_LEFT, speed, duration);
    else if (command == "right") driveFor(MOTOR_STRAFE_RIGHT, speed, duration);
    else if (command == "stop") driveFor(MOTOR_STOP, 0, 0);
    else { setStatus("Неизвестная команда"); sendState(400); }
  });
  // старые имена — чтобы работали прежние скрипты
  server.on("/say", HTTP_POST, handleApiReply);
  server.on("/emo", HTTP_GET, handleApiEmotion);
  server.onNotFound([]() { sendState(404); });
  server.begin();

  setStatus("Готов");
  Serial.println();
  Serial.printf("Клешня %s, прошивка %s\n", ROBOT_NAME, FIRMWARE_VERSION);
  Serial.printf("Рот (128x32): %s\n", mouthFound ? "найден" : "НЕ НАЙДЕН");
  Serial.printf("Глаз (128x64): %s\n", eyeFound ? "найден" : "НЕ НАЙДЕН");
  Serial.printf("Пульт: http://%s\n", WiFi.softAPIP().toString().c_str());
  if (strlen(STA_SSID)) Serial.printf("Сеть ноутбука: %s\n", STA_SSID);
  Serial.println("Ходьба: " + String(movementAllowed ? "включена" : "выключена"));
  if (movementAllowed) {
    Serial.println("--- Драйвер моторов ---");
    Serial.printf("  AIN1=GPIO%d  AIN2=GPIO%d  BIN1=GPIO%d  BIN2=GPIO%d  STBY=GPIO%d\n",
                  PIN_AIN1, PIN_AIN2, PIN_BIN1, PIN_BIN2, PIN_STBY);
    if (PIN_PWMA >= 0) {
      Serial.printf("  PWMA=GPIO%d  PWMB=GPIO%d\n", PIN_PWMA, PIN_PWMB);
    } else {
      Serial.println("  PWMA/PWMB: скорость идёт по AIN1/BIN1");
    }
    Serial.println("  Проверь: VCC драйвера на 3V3, GND общий, STBY=HIGH");
  }
  beepSequence(1, 0);
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  server.handleClient();
  updateMicLevel();
  serviceDiscovery();

  const uint32_t now = millis();

  // Сторожевой таймер «думаю» — больше не зависаем навсегда
  if (thinkDeadline && now >= thinkDeadline) {
    thinkDeadline = 0;
    emotion = EMO_NEUTRAL;
    eyeDirty = true;
    setStatus("Нет ответа. Повтори.");
  }

  // Временная эмоция истекла
  if (emotionUntil && now >= emotionUntil) {
    emotionUntil = 0;
    if (emotion != EMO_THINK) {
      emotion = EMO_NEUTRAL;
      eyeDirty = true;
    }
    if (!replyText.length() || now >= replyUntil) setStatus("Готов");
  }

  // Ответ на экране истёк
  if (replyUntil && now >= replyUntil) {
    replyUntil = 0;
    replyText = "";
    mouthDirty = true;
    if (!thinkDeadline) setStatus("Готов");
  }

  // Обновляем наличие ноутбука раз в 5 секунд
  if (now - lastServiceAt > 5000) {
    lastServiceAt = now;
    const bool wasConnected = brainConnected;
    if (brainKnown()) {
      int status = 0;
      brainRequest("/api/ping", "", nullptr, status);
      brainConnected = (status > 0);
    }
    if (wasConnected != brainConnected) {
      Serial.println(brainConnected ? "Ноутбук на связи" : "Ноутбук потерян");
    }
  }

  refreshDisplays();
}
