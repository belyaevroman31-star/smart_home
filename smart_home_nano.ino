// Умный дом — прошивка для Arduino Nano
// Управляется панелью smart_home.html по USB (Serial).
//
// Библиотеки:
//   - AccelStepper        — шторы (шаговый двигатель)
//   - OneWire + DallasTemperature — DS18B20 для автоматики
//     климата (включаются при USE_TEMP_SENSOR=1)
//
// Пины (пример разводки):
//   D2  нагреватель (реле)
//   D3  LED-подсветка (PWM, яркость)
//   D4  основное освещение (реле)
//   D5  вентилятор кондиционера (PWM, 0..100%)
//   D6  циркуляционный мотор — 12В помпа (MOSFET/реле)
//   D7  питание кондиционера (реле)
//   D8–D11 шаговый двигатель штор (28BYJ-48 + ULN2003)
//   D12 датчик DS18B20 (если USE_TEMP_SENSOR=1)
//
// Протокол (строка + '\n'):
//   L=0|1           основное освещение
//   LED=0..100      яркость подсветки
//   TEMP=15..30     целевая температура отопления
//   PUMP=0|1        циркуляционный мотор
//   AC=0|1          кондиционер вкл/выкл
//   ACMODE=cool|heat|dry|auto
//   ACTEMP=16..30   температура кондиционера
//   ACFAN=0..100    скорость вентилятора кондиционера, %
//   CUR=0..100      шторы (0 открыто, 100 закрыто)
//   SCENE=home|evening|cinema|morning|night|away
//   CMAX=500..20000 калибровка штор (шагов на 100%)
//   HYST=0.1..2.0   порог срабатывания климат-автоматики, °C
//   TS=<unix>       установка часов (эпоха, секунды) — для расписания
//   SCHED=<мин>:<команда>  расписание (0..1439 минут от полуночи)
//   SCHEDDEL=<мин>  удалить пункт расписания по времени
//   HB              heartbeat от панели (сторожевой таймер)
//   HBIT=<сек>      период heartbeat, 0 — сторожевой таймер выключен
//   ?               запрос состояния

#define USE_TEMP_SENSOR 0

#if USE_TEMP_SENSOR
  #include <OneWire.h>
  #include <DallasTemperature.h>
#endif
#include <AccelStepper.h>

const int PIN_HEAT_RELAY = 2;
const int PIN_LED        = 3;
const int PIN_MAIN_LIGHT = 4;
const int PIN_AC_FAN     = 5;
const int PIN_PUMP       = 6;
const int PIN_AC_POWER   = 7;
#if USE_TEMP_SENSOR
const int PIN_SENSOR     = 12;
#endif

AccelStepper curtains(AccelStepper::FULL4WIRE, 8, 9, 10, 11);
long curtainMaxSteps = 4000;   // калибровка: сколько шагов на 100% хода штор (команда CMAX=)
float curtainHysteresis = 0.5f; // порог срабатывания климат-автоматики, °C (команда HYST=)
const float CURTAIN_SPEED = 600;
const float CURTAIN_ACCEL = 800;
int curtainPercent = 0;

#if USE_TEMP_SENSOR
OneWire oneWire(PIN_SENSOR);
DallasTemperature sensor(&oneWire);
float indoorTemp = 0.0f;
#endif

int mainLight = 0;
int ledVal    = 0;
int heatTemp  = 20;
int pumpOn    = 0;
int acOn      = 0;
int acFan     = 100;
String acMode = "cool";
int acTemp    = 22;

// Часы (software): устанавливаются командой TS=<unix> от панели.
unsigned long clockBaseMillis = 0;
unsigned long clockBaseEpoch  = 0;
bool clockSynced = false;

// Расписание: до 8 пунктов, время указывается в минутах от полуночи.
#define SCHED_SLOTS 8
String schedTime[SCHED_SLOTS];
String schedCmd[SCHED_SLOTS];
int lastSchedMinute = -1;

// Heartbeat / сторожевой таймер: если панель не отвечает guardTimeout,
// контроллер сам возвращает сценарий «дома».
unsigned long lastHeartbeat = 0;
unsigned long guardTimeout  = 45000UL;
bool guardEnabled = true;
bool guardSynced  = false;
bool guardTriggered = false;

void setup() {
  Serial.begin(9600);
  pinMode(PIN_HEAT_RELAY, OUTPUT);
  pinMode(PIN_MAIN_LIGHT, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_AC_FAN, OUTPUT);
  pinMode(PIN_PUMP, OUTPUT);
  pinMode(PIN_AC_POWER, OUTPUT);
  digitalWrite(PIN_HEAT_RELAY, LOW);
  digitalWrite(PIN_MAIN_LIGHT, LOW);
  digitalWrite(PIN_LED, LOW);
  digitalWrite(PIN_AC_FAN, LOW);
  digitalWrite(PIN_PUMP, LOW);
  digitalWrite(PIN_AC_POWER, LOW);
#if USE_TEMP_SENSOR
  sensor.begin();
#endif
  curtains.setMaxSpeed(CURTAIN_SPEED);
  curtains.setAcceleration(CURTAIN_ACCEL);
  curtains.setSpeed(CURTAIN_SPEED);
  curtains.setCurrentPosition(0);
}

String serialBuf = "";

// Текущее время в минутах от полуночи (по установленным часам).
int currentMinute() {
  if (!clockSynced) return -1;
  unsigned long epoch = clockBaseEpoch + (millis() - clockBaseMillis) / 1000UL;
  return (int)((epoch / 60UL) % 1440UL);
}

void checkSchedule() {
  int nowMin = currentMinute();
  if (nowMin < 0 || nowMin == lastSchedMinute) return;
  lastSchedMinute = nowMin;
  for (int i = 0; i < SCHED_SLOTS; i++) {
    if (schedTime[i].length() == 0) continue;
    if (schedTime[i].toInt() == nowMin) {
      Serial.print("SCHED="); Serial.println(schedCmd[i]);
      handleCommand(schedCmd[i]);
    }
  }
}

void checkGuard() {
  if (!guardEnabled || !guardSynced) return;
  if (guardTriggered) return;
  if (millis() - lastHeartbeat > guardTimeout) {
    guardTriggered = true;
    Serial.println("GUARD=home");
    runScene("home");
  }
}

void loop() {
  while (Serial.available() > 0) {
    char ch = (char)Serial.read();
    if (ch == '\n') {
      serialBuf.trim();
      if (serialBuf.length() > 0) handleCommand(serialBuf);
      serialBuf = "";
    } else if (ch != '\r') {
      serialBuf += ch;
      if (serialBuf.length() > 100) serialBuf = "";
    }
  }

  curtains.moveTo((long)curtainPercent * curtainMaxSteps / 100L);
  if (curtains.distanceToGo() != 0) curtains.run();

  checkSchedule();
  checkGuard();

#if USE_TEMP_SENSOR
  static unsigned long lastRead = 0;
  if (millis() - lastRead > 3000) {
    lastRead = millis();
    sensor.requestTemperatures();
    indoorTemp = sensor.getTempCByIndex(0);
    updateClimate();
  }
#endif
}

void handleCommand(String line) {
  int eq = line.indexOf('=');
  if (eq < 0) {
    if (line == "?") sendStatus();
    else if (line == "HB") {
      // heartbeat от панели: сбрасываем сторожевой таймер
      lastHeartbeat = millis();
      guardSynced = true;
      guardTriggered = false;
    }
    return;
  }
  String key = line.substring(0, eq);
  String val = line.substring(eq + 1);
  int iv = val.toInt();

  if      (key == "L")       mainLightSet(iv);
  else if (key == "LED")     ledSet(constrain(iv, 0, 100));
  else if (key == "TEMP")    heatTemp = constrain(iv, 15, 30);
  else if (key == "PUMP")    pumpSet(iv);
  else if (key == "AC")      acPowerSet(iv);
  else if (key == "ACTEMP")  acTemp = constrain(iv, 16, 30);
  else if (key == "ACMODE")  { acMode = val; acMode.trim(); }
  else if (key == "ACFAN")   { acFan = constrain(iv, 0, 100); applyACFan(); }
  else if (key == "CUR")     { curtainPercent = constrain(iv, 0, 100); curtains.moveTo((long)curtainPercent * curtainMaxSteps / 100L); }
  else if (key == "SCENE")   runScene(val);
  else if (key == "CMAX")    curtainMaxSteps = constrain(iv, 500, 20000);
  else if (key == "HYST")    curtainHysteresis = constrain(val.toFloat(), 0.1f, 2.0f);
  else if (key == "TS")      { clockBaseEpoch = val.toInt(); clockBaseMillis = millis(); clockSynced = true; }
  else if (key == "HBIT")    { guardTimeout = (unsigned long)constrain(iv, 0, 3600) * 1000UL; guardEnabled = (iv > 0); if (!guardEnabled) guardTriggered = false; }
  else if (key == "SCHEDDEL") { if (val == "*") clearSchedule(); else removeSchedule(val.toInt()); }
  else if (key == "SCHED")   addSchedule(val);
}

void clearSchedule() {
  for (int i = 0; i < SCHED_SLOTS; i++) {
    schedTime[i] = "";
    schedCmd[i] = "";
  }
}

void addSchedule(String val) {
  // Формат: <минуты от полуночи>:<команда>, например "420:SCENE=morning"
  int colon = val.indexOf(':');
  if (colon < 0) return;
  String mins = val.substring(0, colon);
  String cmd = val.substring(colon + 1);
  int m = mins.toInt();
  if (m < 0 || m > 1439 || cmd.length() == 0) return;
  for (int i = 0; i < SCHED_SLOTS; i++) {
    if (schedTime[i].length() > 0 && schedTime[i].toInt() == m) {
      schedCmd[i] = cmd; // перезаписываем пункт на то же время
      return;
    }
  }
  for (int i = 0; i < SCHED_SLOTS; i++) {
    if (schedTime[i].length() == 0) {
      schedTime[i] = mins;
      schedCmd[i] = cmd;
      return;
    }
  }
  // место закончилось — заменяем самый ранний пункт
  schedTime[0] = mins;
  schedCmd[0] = cmd;
}

void removeSchedule(int minute) {
  for (int i = 0; i < SCHED_SLOTS; i++) {
    if (schedTime[i].length() > 0 && schedTime[i].toInt() == minute) {
      schedTime[i] = "";
      schedCmd[i] = "";
    }
  }
}

void mainLightSet(int on) {
  mainLight = on ? 1 : 0;
  digitalWrite(PIN_MAIN_LIGHT, mainLight);
}

void ledSet(int p) {
  ledVal = p;
  analogWrite(PIN_LED, map(p, 0, 100, 0, 255));
}

void pumpSet(int on) {
  pumpOn = on ? 1 : 0;
  digitalWrite(PIN_PUMP, pumpOn);
}

void acPowerSet(int on) {
  acOn = on ? 1 : 0;
  // Защита от параллельного включения: нагрев и кондиционер не работают вместе
  if (acOn) {
    digitalWrite(PIN_HEAT_RELAY, LOW);
    if (acMode == "cool" || acMode == "auto") heatTemp = min(heatTemp, acTemp - 1);
  }
  digitalWrite(PIN_AC_POWER, acOn);
  if (!acOn) analogWrite(PIN_AC_FAN, 0);
  else applyACFan();
}

void applyACFan() {
  if (!acOn) return;
  int pwm = map(constrain(acFan, 0, 100), 0, 100, 0, 255);
  analogWrite(PIN_AC_FAN, pwm);
}

// Автоматика по датчику температуры (опционально)
#if USE_TEMP_SENSOR
void updateClimate() {
  // Отопление: стало холоднее целевой температуры → греем и гоним мотор
  if (indoorTemp <= heatTemp - curtainHysteresis) {
    // Защита от параллельного включения: при работающем кондиционере не греем
    if (acOn == 0) {
      digitalWrite(PIN_HEAT_RELAY, HIGH);
      pumpSet(1);
    }
  } else if (indoorTemp >= heatTemp + curtainHysteresis) {
    digitalWrite(PIN_HEAT_RELAY, LOW);
    if (acOn == 0) pumpSet(0);
  }
  // Кондиционер в режиме охлаждения: теплее целевой температуры → вентилятор
  if (acOn && (acMode == "cool" || acMode == "auto")) {
    if (indoorTemp >= acTemp + curtainHysteresis) applyACFan();
    else analogWrite(PIN_AC_FAN, 0);
  }
}
#endif

void runScene(String name) {
  if      (name == "home")    { mainLightSet(1); ledSet(75); heatTemp = 24; pumpSet(1); curtainPercent = 0; }
  else if (name == "evening") { mainLightSet(0); ledSet(50); heatTemp = 21; curtainPercent = 100; }
  else if (name == "cinema")  { curtainPercent = 100; ledSet(25); }
  else if (name == "morning") { curtainPercent = 0; ledSet(40); heatTemp = 22; }
  else if (name == "night")   { mainLightSet(0); ledSet(0); heatTemp = 19; curtainPercent = 100; }
  else if (name == "away")    { mainLightSet(0); ledSet(0); heatTemp = 19; pumpSet(0); acPowerSet(0); acFan = 100; curtainPercent = 0; }
}

void sendStatus() {
  Serial.print("L="); Serial.print(mainLight);
  Serial.print(";LED="); Serial.print(ledVal);
  Serial.print(";TEMP="); Serial.print(heatTemp);
  Serial.print(";PUMP="); Serial.print(pumpOn);
  Serial.print(";AC="); Serial.print(acOn);
  Serial.print(";ACTEMP="); Serial.print(acTemp);
  Serial.print(";ACMODE="); Serial.print(acMode);
  Serial.print(";ACFAN="); Serial.print(acFan);
  Serial.print(";CUR="); Serial.print(curtainPercent);
  Serial.print(";CMAX="); Serial.print(curtainMaxSteps);
  Serial.print(";HYST="); Serial.print((int)(curtainHysteresis * 10.0f + 0.5f));  // °C × 10
#if USE_TEMP_SENSOR
  Serial.print(";INDOOR="); Serial.print(indoorTemp, 1);
#endif
  Serial.println();
}