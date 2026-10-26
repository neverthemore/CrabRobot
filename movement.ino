// ============================================================
//  КЛЕШНЯ 2.0 — ХОДЬБА (отдельный файл)
//
//  Два DC-мотора с редуктором, у каждого только «+» и «−».
//  Управляются двухканальным H-мостом:
//
//    AIN1 / AIN2  →  мотор А (левая сторона ног)  →  AO1 / AO2
//    BIN1 / BIN2  →  мотор Б (правая сторона ног) →  BO1 / BO2
//    STBY         →  включение драйвера (HIGH = готов работать)
//
//  ДВА ВАРИАНТА УПРАВЛЕНИЯ СКОРОСТЬЮ (выбор в config.h):
//
//  1) Есть выводы PWMA / PWMB на плате драйвера:
//       PIN_PWMA = 18, PIN_PWMB = 19   (или другие свободные пины)
//     Тогда AIN1/AIN2 и BIN1/BIN2 — обычный GPIO (направление),
//     а скорость идёт отдельным ШИМом на PWMA/PWMB.
//     Это ПРАВИЛЬНЫЙ режим для TB6612 и почти всех модулей.
//
//  2) Нет выводов PWMA / PWMB:
//       PIN_PWMA = -1, PIN_PWMB = -1
//     Тогда ШИМ подаётся прямо на AIN1/BIN1, а второй вход канала
//     держится на нуле. Многие драйверы так умеют, но не все.
//
//  ВАЖНО про ESP32 core 3.x: нельзя одновременно писать в пин через
//  digitalWrite() и через ledcWrite(). После ledcAttach() пин больше
//  не GPIO, и digitalWrite() молча ничего не делает.
//  Поэтому в этом файле на пинах направления используется ТОЛЬКО
//  ledcWrite() — никаких digitalWrite() на них.
//
//  Направление: «+» мотора на AO1/BO1 — вперёд, на AO2/BO2 — назад.
//  Если «Вперёд» уезжает назад — поменяй MOTOR_B_REVERSED в config.h
//  или переставь провода мотора.
// ============================================================

#include "hardware.h"

static bool motorsReady = false;
static bool stbyEnabled = false;

// ---------- Направление «+» мотора ----------
// Тип MotorDir объявлен в hardware.h (там объяснение порядка .ino-файлов).

static int speedToDuty(int speedPercent) {
  speedPercent = constrain(speedPercent, 0, 100);
  if (speedPercent < MOTOR_MIN_SPEED) return 0;
  return map(speedPercent, 0, 100, 0, 255);
}

// ---------- Совместимость с разными версиями ядра ESP32 ----------
// Ядро 3.x: ledcAttach(пин, частота, разрешение) + ledcWrite(пин, duty).
// Ядро 2.x: ledcSetup(канал, частота, разрешение) + ledcAttachPin(пин, канал).
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define LEDC_SETUP(pin, freq, bits) ledcAttach(pin, freq, bits)
  #define LEDC_WRITE(pin, duty)        ledcWrite(pin, duty)
#else
  static uint8_t ledcChannel = 0;
  #define LEDC_SETUP(pin, freq, bits) do { ledcSetup(ledcChannel, freq, bits); ledcAttachPin(pin, ledcChannel); ledcChannel++; } while (0)
  #define LEDC_WRITE(pin, duty)        ledcWrite(pin, duty)
#endif

// Управление одним мотором.
//
// in1 / in2 — контакты направления канала.
// pwmPin    — отдельный вывод скорости (PWMA/PWMB), либо -1.
// dir       — DIR_FWD / DIR_REV / DIR_STOP.
// duty      — ШИМ 0..255.
//
// ВАЖНО: если pwmPin == -1, скорость идёт на сам in1/in2,
// и тогда digitalWrite() по ним недопустим (пин принадлежит LEDC).
static void applyMotor(uint8_t in1, uint8_t in2, int pwmPin,
                       MotorDir dir, int duty) {
  if (pwmPin >= 0) {
    // ---- Режим 1: отдельный пин скорости ----
    // Направление — обычный GPIO, скорость — на PWMA.
    if (dir == DIR_FWD) {
      digitalWrite(in1, HIGH);
      digitalWrite(in2, LOW);
    } else if (dir == DIR_REV) {
      digitalWrite(in1, LOW);
      digitalWrite(in2, HIGH);
    } else {
      digitalWrite(in1, LOW);
      digitalWrite(in2, LOW);
    }
    LEDC_WRITE((uint8_t)pwmPin, (dir == DIR_STOP) ? 0 : duty);
    return;
  }

  // ---- Режим 2: скорость идёт на IN1/IN2 ----
  // Обе ноги — под LEDC, поэтому только ledcWrite().
  // Управляющая нога: ШИМ. Вторая нога: постоянный ноль (= LOW).
  if (dir == DIR_FWD) {
    LEDC_WRITE(in1, duty);
    LEDC_WRITE(in2, 0);
  } else if (dir == DIR_REV) {
    LEDC_WRITE(in1, 0);
    LEDC_WRITE(in2, duty);
  } else {
    LEDC_WRITE(in1, 0);
    LEDC_WRITE(in2, 0);
  }
}

void setupMotors() {
  if (!MOVEMENT_ENABLED) {
    // Пины моторов даже не переключаем в OUTPUT — драйвер не трогаем.
    Serial.println("Ходьба выключена (config.h: MOVEMENT_ENABLED = false)");
    return;
  }

  // ВАЖНЫЙ ПОРЯДОК: сначала отдаём пины под ШИМ (ledcAttach),
  // потом переключаем в OUTPUT только те пины, которые остаются GPIO.
  // На ESP32 core 3.x пину нельзя одновременно быть GPIO и LEDC.

  if (PIN_PWMA >= 0) {
    // ---- Режим 1: отдельные пины скорости (PWMA / PWMB) ----
    // Направление остаётся обычным GPIO.
    LEDC_SETUP((uint8_t)PIN_PWMA, PWM_FREQ, PWM_BITS);
    LEDC_SETUP((uint8_t)PIN_PWMB, PWM_FREQ, PWM_BITS);

    pinMode(PIN_AIN1, OUTPUT);
    pinMode(PIN_AIN2, OUTPUT);
    pinMode(PIN_BIN1, OUTPUT);
    pinMode(PIN_BIN2, OUTPUT);
    digitalWrite(PIN_AIN1, LOW);
    digitalWrite(PIN_AIN2, LOW);
    digitalWrite(PIN_BIN1, LOW);
    digitalWrite(PIN_BIN2, LOW);
    Serial.printf("Моторы: скорость через PWMA=GPIO%d, PWMB=GPIO%d\n",
                  PIN_PWMA, PIN_PWMB);
  } else {
    // ---- Режим 2: скорость идёт на AIN1 / BIN1 ----
    // Все четыре ноги — под LEDC, digitalWrite() по ним запрещён.
    LEDC_SETUP(PIN_AIN1, PWM_FREQ, PWM_BITS);
    LEDC_SETUP(PIN_AIN2, PWM_FREQ, PWM_BITS);
    LEDC_SETUP(PIN_BIN1, PWM_FREQ, PWM_BITS);
    LEDC_SETUP(PIN_BIN2, PWM_FREQ, PWM_BITS);
    Serial.println("Моторы: скорость идёт на AIN1/BIN1 (отдельных PWM-пинов нет)");
  }

  // STBY: драйвер молчит, пока сюда не придёт HIGH
  pinMode(PIN_STBY, OUTPUT);
  digitalWrite(PIN_STBY, LOW);

  motorsReady = true;
  digitalWrite(PIN_STBY, HIGH);   // драйвер включён
  stbyEnabled = true;
  Serial.println("Ходьба: драйвер включён (STBY = HIGH)");
}

bool movementEnabled() {
  return motorsReady;
}

void motorsStop() {
  if (!motorsReady) return;
  applyMotor(PIN_AIN1, PIN_AIN2, PIN_PWMA, DIR_STOP, 0);
  applyMotor(PIN_BIN1, PIN_BIN2, PIN_PWMB, DIR_STOP, 0);
}

// Включить/выключить драйвер целиком (STBY)
void motorsStandby(bool on) {
  if (!motorsReady) return;
  if (on) {
    digitalWrite(PIN_STBY, HIGH);
    stbyEnabled = true;
  } else {
    motorsStop();
    digitalWrite(PIN_STBY, LOW);
    stbyEnabled = false;
  }
}

bool motorsStandbyState() {
  return stbyEnabled;
}

// ---------- Движения ----------
// Моторы стоят зеркально: правый крутится в обратную сторону,
// иначе робот вместо езды вперёд крутится на месте.
void driveForward(int speedPercent) {
  if (!motorsReady) return;
  const int duty = speedToDuty(speedPercent);
  applyMotor(PIN_AIN1, PIN_AIN2, PIN_PWMA, DIR_FWD, duty);
  applyMotor(PIN_BIN1, PIN_BIN2, PIN_PWMB,
             MOTOR_B_REVERSED ? DIR_REV : DIR_FWD, duty);
}

void driveBackward(int speedPercent) {
  if (!motorsReady) return;
  const int duty = speedToDuty(speedPercent);
  applyMotor(PIN_AIN1, PIN_AIN2, PIN_PWMA, DIR_REV, duty);
  applyMotor(PIN_BIN1, PIN_BIN2, PIN_PWMB,
             MOTOR_B_REVERSED ? DIR_FWD : DIR_REV, duty);
}

// Боком: моторы крутятся в разные стороны, корпус поворачивается
void driveStrafeLeft(int speedPercent) {
  if (!motorsReady) return;
  const int duty = speedToDuty(speedPercent);
  applyMotor(PIN_AIN1, PIN_AIN2, PIN_PWMA, DIR_REV, duty);
  applyMotor(PIN_BIN1, PIN_BIN2, PIN_PWMB,
             MOTOR_B_REVERSED ? DIR_FWD : DIR_REV, duty);
}

void driveStrafeRight(int speedPercent) {
  if (!motorsReady) return;
  const int duty = speedToDuty(speedPercent);
  applyMotor(PIN_AIN1, PIN_AIN2, PIN_PWMA, DIR_FWD, duty);
  applyMotor(PIN_BIN1, PIN_BIN2, PIN_PWMB,
             MOTOR_B_REVERSED ? DIR_REV : DIR_FWD, duty);
}

// ---------- Диагностика: крутим один мотор в одну сторону ----------
// motor: 0 = мотор А (AO1/AO2), 1 = мотор Б (BO1/BO2)
// forward: true = «+» мотора на AO1/BO1, false = на AO2/BO2
void driveOneMotor(int motor, bool forward, int speedPercent) {
  if (!motorsReady) return;
  const int duty = speedToDuty(speedPercent);
  if (motor == 0) {
    applyMotor(PIN_AIN1, PIN_AIN2, PIN_PWMA, forward ? DIR_FWD : DIR_REV, duty);
  } else {
    applyMotor(PIN_BIN1, PIN_BIN2, PIN_PWMB, forward ? DIR_FWD : DIR_REV, duty);
  }
}
