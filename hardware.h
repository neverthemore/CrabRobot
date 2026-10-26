// ============================================================
//  КЛЕШНЯ 2.0 — общие объявления и состояние (hardware.h)
//  Arduino IDE склеивает все .ino одного скетча в один файл,
//  .h подключаются отдельно. Здесь — то, что видят все части.
// ============================================================
#pragma once
#include <Arduino.h>
#include "config.h"

// ---------- ЭМОЦИИ ----------
enum Emotion { EMO_NEUTRAL, EMO_HAPPY, EMO_SAD, EMO_WOW, EMO_THINK };

// ---------- ОБЩЕЕ СОСТОЯНИЕ ----------
extern Emotion emotion;
extern String statusText;
extern String replyText;
extern uint32_t replyStartedAt;
extern uint32_t replyUntil;
extern uint32_t emotionUntil;
extern uint32_t thinkDeadline;   // 0 = не «думает»
extern bool mouthFound;
extern bool eyeFound;
extern bool brainConnected;
extern String brainHostName;
extern String lastQuestion;
extern int micLevel;
extern int micNoiseFloor;
extern bool movementAllowed;

// ---------- Общий разбор простого JSON ----------
String jsonField(const String& json, const String& key);
String jsonEscape(const String& value);

// ---------- СЕРВОПРИВОДЫ ----------
void setupServos();
void setLeftArm(int angle);
void setRightArm(int angle);
void setLeftClaw(int angle);
void setRightClaw(int angle);
void homePose();
void stopServos();

// ---------- ЭМОЦИИ / СТАТУС / ЗВУК ----------
void setEmotion(Emotion next, bool withSound);
void setTemporaryEmotion(Emotion next, uint32_t durationMs, bool withSound);
void setStatus(const String& text);
String emotionLabel(Emotion value);
bool emotionFromName(const String& name, Emotion& out);
void beep(uint16_t durationMs);
void beepSequence(uint8_t count, uint16_t gapMs);
void silenceBuzzer();
void startThinking(uint32_t timeoutMs);
void stopThinking();

// ---------- ЭКРАНЫ ----------
void setupDisplays();
void drawFace();
void showReplyText(const String& text);
void refreshDisplays();

// ---------- МИКРОФОН ----------
void setupMicrophone();
bool microphoneBusy();
void startMicrophoneRecording();
void stopMicrophoneRecording();
void updateMicLevel();

// ---------- МОТОРЫ (movement.ino) ----------
enum MotorCommand { MOTOR_STOP, MOTOR_FORWARD, MOTOR_BACKWARD, MOTOR_STRAFE_LEFT, MOTOR_STRAFE_RIGHT };
// Направление «+» мотора. Объявлено здесь, а не в movement.ino:
// Arduino IDE вставляет прототипы функций в начало первого .ino —
// если тип живёт в .ino, прототип оказывается раньше определения.
enum MotorDir { DIR_STOP, DIR_FWD, DIR_REV };
void setupMotors();
void motorsStop();
void driveForward(int speedPercent);
void driveBackward(int speedPercent);
void driveStrafeLeft(int speedPercent);
void driveStrafeRight(int speedPercent);
bool movementEnabled();
// Диагностика: один мотор в одну сторону (motor: 0 = А, 1 = Б)
void driveOneMotor(int motor, bool forward, int speedPercent);
void motorsStandby(bool on);
bool motorsStandbyState();

// ---------- СЕТЬ / МОЗГ ----------
void setupWiFi();
String brainHost();
bool brainKnown();
void brainSetHost(const String& host);
void pushEmotionToBrain(const String& emotionName);
void pushReplyToBrain(const String& text, const String& emotionName);
String brainRequest(const String& path, const String& body, const char* contentType, int& status);
String brainRequestBinary(const String& path, uint8_t* data, size_t length,
                          const char* contentType, int& status);
