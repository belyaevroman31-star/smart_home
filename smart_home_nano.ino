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
//   D5  вентилятор кондиционера (PWM, 3 скорости)
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
//   ACFAN=1..3      скорость вентилятора
//   CUR=0..100      шторы (0 открыто, 100 закрыто)
//   SCENE=home|evening|cinema|morning|night|away
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
const long CURTAIN_MAX_STEPS = 4000;
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
int acFan     = 1;
String acMode = "cool";
int acTemp    = 22;

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

  curtains.moveTo((long)curtainPercent * CURTAIN_MAX_STEPS / 100L);
  if (curtains.distanceToGo() != 0) curtains.run();

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
  else if (key == "ACFAN")   { acFan = constrain(iv, 1, 3); applyACFan(); }
  else if (key == "CUR")     { curtainPercent = constrain(iv, 0, 100); curtains.moveTo((long)curtainPercent * CURTAIN_MAX_STEPS / 100L); }
  else if (key == "SCENE")   runScene(val);
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
  digitalWrite(PIN_AC_POWER, acOn);
  if (!acOn) analogWrite(PIN_AC_FAN, 0);
  else applyACFan();
}

void applyACFan() {
  if (!acOn) return;
  int pwm = (acFan == 1) ? 170 : (acFan == 2) ? 215 : 255;
  analogWrite(PIN_AC_FAN, pwm);
}

// Автоматика по датчику температуры (опционально)
#if USE_TEMP_SENSOR
void updateClimate() {
  // Отопление: стало холоднее целевой температуры → греем и гоним мотор
  if (indoorTemp <= heatTemp - 0.5f) {
    digitalWrite(PIN_HEAT_RELAY, HIGH);
    pumpSet(1);
  } else if (indoorTemp >= heatTemp + 0.5f) {
    digitalWrite(PIN_HEAT_RELAY, LOW);
    if (acOn == 0) pumpSet(0);
  }
  // Кондиционер в режиме охлаждения: теплее целевой температуры → вентилятор
  if (acOn && (acMode == "cool" || acMode == "auto")) {
    if (indoorTemp >= acTemp + 0.5f) applyACFan();
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
  else if (name == "away")    { mainLightSet(0); ledSet(0); heatTemp = 19; pumpSet(0); acPowerSet(0); acFan = 1; curtainPercent = 100; }
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
#if USE_TEMP_SENSOR
  Serial.print(";INDOOR="); Serial.print(indoorTemp, 1);
#endif
  Serial.println();
}