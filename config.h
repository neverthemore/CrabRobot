// ============================================================
//  КЛЕШНЯ — общие настройки (config.h)
//  Всё, что можно менять при сборке, — здесь.
//  Меняешь только этот файл, остальные .ino трогать не нужно.
// ============================================================
#pragma once
#include <Arduino.h>

// ---------- ПИТАНИЕ / ВЕРСИЯ ----------
constexpr char FIRMWARE_VERSION[] = "2.0";
constexpr char ROBOT_NAME[] = "Клешня";

// ---------- OLED «РОТ» (128x32) + AS5600 — общая шина I2C ----------
constexpr uint8_t PIN_I2C_SDA = 21;
constexpr uint8_t PIN_I2C_SCL = 22;
constexpr uint8_t OLED_ADDRESS = 0x3C;
constexpr uint8_t AS5600_ADDRESS = 0x36;

// ---------- OLED «ГЛАЗ» (128x64) — программная шина (bit-bang) ----------
constexpr uint8_t PIN_EYE_SDA = 4;
constexpr uint8_t PIN_EYE_SCL = 5;

// ---------- МИКРОФОН MAX9814 (аналоговый выход) ----------
constexpr uint8_t PIN_MIC = 34;

// ---------- ЗУММЕР (пассивный, с выводом IN) ----------
constexpr uint8_t PIN_BUZZER = 19;
constexpr bool  BUZZER_ENABLED = true;   // выключи константой false, если зуммер не нужен
// true  — активный зуммер (только VCC и GND, писк = постоянный уровень)
// false — пассивный зуммер с выводом IN (писк = меандр, как в большинстве модулей)
constexpr bool  BUZZER_IS_ACTIVE = false;
constexpr bool  EMOTION_SOUND = false;   // по умолчанию Клешня молчит: пищит только кнопка «Зуммер»
constexpr uint16_t BUZZER_FREQ = 2000;   // Гц, приятный тон вместо гудения
constexpr uint16_t BUZZER_CHIRP_MS = 70;  // длительность одного «чирпа»

// ---------- СЕРВОПРИВОДЫ КЛЕШНЕЙ ----------
constexpr uint8_t PIN_SRV_LARM  = 13;   // левая рука (подъём)
constexpr uint8_t PIN_SRV_LCLAW = 25;   // левая клешня (щипок)
constexpr uint8_t PIN_SRV_RARM  = 27;   // правая рука
constexpr uint8_t PIN_SRV_RCLAW = 26;   // правая клешня
constexpr uint16_t SERVO_MIN_US = 500;
constexpr uint16_t SERVO_MAX_US = 2400;
constexpr uint16_t SERVO_PERIOD_HZ = 50;

// Домашняя поза (угол серво: 0 = вниз, 150 = вверх; клешня 20 = сжата, 90 = раскрыта)
constexpr int HOME_LARM = 10;
constexpr int HOME_RARM = 10;
constexpr int HOME_LCLAW = 20;
constexpr int HOME_RCLAW = 20;

// ---------- ДРАЙВЕР МОТОРОВ (TB6612FNG, 16 контактов) ----------
// Распиновка разъёма драйвера (сверху вниз по рядам):
//   NC AIN2 AIN1 STBY BIN1 BIN2 | NC GND VM | NC GND | AO1 AO2 BO2 BO1 GND
//
// Три контакта, подписанные NC, — это на самом деле:
//   VCC  — питание ЛОГИКИ драйвера. ОБЯЗАТЕЛЬНО → 3V3 ESP32.
//          Без него драйвер не работает вообще (молчат оба мотора).
//   PWMA — вход ШИМа для канала A
//   PWMB — вход ШИМа для канала B
//
// Два варианта подключения — выбери один и настрой константы ниже:
//
// ВАРИАНТ 1 (рекомендуемый): PWMA и PWMB доступны.
//   ESP32 GPIO17 → PWMA
//   ESP32 GPIO33 → PWMB
//   Скорость идёт на отдельные пины, AIN1/AIN2/BIN1/BIN2 — направление.
//   В config.h: PIN_PWMA = 17, PIN_PWMB = 33
//
// ВАРИАНТ 2: PWMA и PWMB недоступны (или уже запаяны на +V).
//   Тогда соедини PWMA и PWMB с 3V3 (перемычкой на плате или проводом).
//   Скорость будет регулироваться ШИМом по AIN1/BIN1.
//   В config.h: PIN_PWMA = -1, PIN_PWMB = -1
//
// ВАЖНО: у TB6612 канал не поедет, если его PWM-вход (PWMA/PWMB) не HIGH.
// В варианте 2 это решается перемычкой на 3V3.
constexpr uint8_t PIN_AIN1 = 23;   // направление мотора А (левая сторона ног)
constexpr uint8_t PIN_AIN2 = 32;
constexpr uint8_t PIN_BIN1 = 14;   // направление мотора Б (правая сторона ног)
constexpr uint8_t PIN_BIN2 = 16;
constexpr uint8_t PIN_STBY = 18;   // драйвер включён = HIGH

// Отдельные пины PWM (PWMA / PWMB). -1 = скорость идёт по AIN1/BIN1.
constexpr int PIN_PWMA = 17;
constexpr int PIN_PWMB = 33;

// ---------- ХОДЬБА ----------
// Пока ходьба не настроена — держи false: страница пульта не покажет кнопки движения,
// а прошивка не будет трогать моторы. Включай отдельным шагом в гайде.
constexpr bool MOVEMENT_ENABLED = true;
// Моторы стоят зеркально: правый крутится в другую сторону.
// Если «Вперёд» уезжает назад — поменяй true <-> false.
constexpr bool MOTOR_B_REVERSED = true;

// Скорость: 0..100 (процент). Ниже MOTOR_MIN_SPEED мотор не трогается.
constexpr int MOTOR_MIN_SPEED = 20;
constexpr int MOTOR_MAX_SPEED = 100;

// ---------- СЕТЬ ----------
constexpr char AP_SSID[] = "CRAB-ROBOT";
constexpr char AP_PASS[] = "crab12345";

// Wi-Fi ноутбука/роутера для режима STA.
// Если оставить пустым — ESP32 поднимет только свою точку CRAB-ROBOT.
// Рекомендуемый режим: ESP32, телефон и ноутбук в одной сети роутера.
constexpr char STA_SSID[] = "CRAB-ROBOT";
constexpr char STA_PASS[] = "crab12345";

// ---------- СВЯЗЬ С НОУТБУКОМ (мозг) ----------
// Если оставить пустым — ESP32 сам найдёт ноутбук по UDP- beacons (автопоиск).
// Иначе можно указать вручную: brain_v2.py --host-ip 192.168.1.10
constexpr char BRAIN_HOST[] = "";     // IP ноутбука
constexpr uint16_t BRAIN_PORT = 8000;
constexpr bool BRAIN_USE_HTTPS = true;  // brain_v2.py работает по HTTPS с самоподписанным сертификатом

// Порт UDP для автопоиска (ESP32 ↔ ноутбук)
constexpr uint16_t DISCOVERY_PORT = 7777;

// ---------- ЗАПИСЬ МИКРОФОНА ----------
constexpr uint16_t MIC_SAMPLE_RATE = 8000;   // ADC ESP32 стабильно тянет 8 кГц (16 кГц не успевает)
constexpr uint16_t MIC_MAX_SECONDS = 4;      // максимум записи
constexpr uint16_t MIC_MIN_SECONDS = 1;      // меньше не слушаем
constexpr uint16_t MIC_SILENCE_MS = 700;     // тишина после речи — стоп записи
constexpr uint16_t MIC_STARTUP_MS = 400;     // сколько ждём начала речи
constexpr int     MIC_NOISE_GATE = 150;      // порог «шум/не шум» (размах отсчёта)
constexpr uint16_t MIC_COOLDOWN_MS = 2500;   // пауза между записями
constexpr bool    MIC_AUTO_START = true;    // true: включать микрофон по громкости

// ---------- ЭМОЦИИ И ТАЙМЕРЫ ----------
constexpr uint16_t EMOTION_SOUND_MS = 2200;   // длительность «эмоции» без звука
constexpr uint16_t THINK_TIMEOUT_MS = 12000;  // если задумалась и ответа нет — вернуться в нейтраль
constexpr uint16_t REPLY_MIN_MS = 4000;       // минимальное время показа ответа на экране
constexpr uint16_t REPLY_MS_PER_CHAR = 90;    // + на каждую букву

// ---------- ПОРТ УПРАВЛЕНИЯ МОТОРАМИ ----------
constexpr int PWM_FREQ = 20000;      // 20 кГц, как требует L298M/DRV8833
constexpr int PWM_BITS = 8;          // 0..255
