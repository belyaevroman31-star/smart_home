// Умный дом — прошивка для Arduino Nano + Wi-Fi (ESP8266 ESP-01)
// PANEL (smart_home.html) → HTTP запросы → ESP8266 → UART → Arduino Nano → модули
//
// Сеть:
//   Режим A (по умолчанию): ESP-01 создаёт собственную точку доступа.
//       SSID: SmartHome   Пароль: 12345678   IP модуля: 192.168.4.1
//       Панель подключается к Wi-Fi «SmartHome» и работает с адресом 192.168.4.1.
//   Режим B: модуль подключается к домашнему роутеру (см. WIFI_STA_SSID/PASS).
//       В этом режиме адрес модуля выдаёт роутер (смотреть в AT+CIFSR).
//
// HTTP API (порт 80):
//   GET /cmd/КОМАНДА  → выполнить команду, ответить текущим состоянием.
//       Примеры: /cmd/L=1, /cmd/TEMP=24, /cmd/SCENE=home, /cmd/CUR=50
//   GET /status        → ответить текущим состоянием
//   GET /              → то же, состояние
//   Ответ тела:  L=..;LED=..;TEMP=..;PUMP=..;AC=..;ACTEMP=..;ACMODE=..;ACFAN=..;CUR=..
//
// Соединение ESP-01 ↔ Nano (SoftwareSerial):
//   Nano A2 (RX, pin16) ← ESP TX
//   Nano A3 (TX, pin17) → ESP RX
//   ESP VCC → +3.3V (стабилизатор!), CH_PD → +3.3V, GND → общий
//
// Библиотеки:
//   AccelStepper — шторы. OneWire + DallasTemperature — DS18B20 (при USE_TEMP_SENSOR=1)

#define USE_TEMP_SENSOR 0

#define WIFI_MODE_AP 1   // 1 = модуль раздаёт сеть; 0 = модуль подключается к роутеру

#define WIFI_AP_SSID  "SmartHome"
#define WIFI_AP_PASS  "12345678"
#define WIFI_STA_SSID "SSID"     // заполнить при WIFI_MODE_AP=0
#define WIFI_STA_PASS "PASS"     // заполнить при WIFI_MODE_AP=0

#include <SoftwareSerial.h>
#include <AccelStepper.h>
#if USE_TEMP_SENSOR
  #include <OneWire.h>
  #include <DallasTemperature.h>
#endif

const int PIN_HEAT_RELAY  = 2;   // нагреватель (реле CH1)
const int PIN_LED         = 3;   // LED-подсветка (PWM)
const int PIN_MAIN_LIGHT  = 4;   // основное освещение (реле CH2)
const int PIN_AC_FAN      = 5;   // вентилятор кондиционера (PWM)
const int PIN_PUMP        = 6;   // циркуляционная помпа 12В (реле CH3)
const int PIN_AC_POWER    = 7;   // питание кондиционера (реле CH4)

const int ESP_RX_PIN = A2;  // 16 — от ESP TX
const int ESP_TX_PIN = A3;  // 17 — к ESP RX

SoftwareSerial esp(ESP_RX_PIN, ESP_TX_PIN);

// --- Шторы (28BYJ-48 + ULN2003, D8–D11) ---
AccelStepper curtains(AccelStepper::FULL4WIRE, 8, 9, 10, 11);
const long CURTAIN_MAX_STEPS = 4000;
const float CURTAIN_SPEED = 600;
const float CURTAIN_ACCEL = 800;
int curtainPercent = 0;

#if USE_TEMP_SENSOR
  const int PIN_SENSOR = 12;
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

// ------------------------------------------------------------------
// ESP-01: AT команды
// ------------------------------------------------------------------
bool espWaitFor(String okTag, unsigned long timeout) {
  String buf = "";
  unsigned long t0 = millis();
  while (millis() - t0 < timeout) {
    while (esp.available()) {
      char c = esp.read();
      if (c == '\r') continue;
      buf += c;
      if (buf.length() > 64) buf = buf.substring(buf.length() - 64);
      if (buf.indexOf(okTag) >= 0) return true;
      if (buf.indexOf("ERROR") >= 0) return false;
    }
  }
  return false;
}

bool espCmd(const __FlashStringHelper* cmd, unsigned long timeout) {
  esp.print(cmd);
  return espWaitFor("OK", timeout);
}

bool espCmdStr(const String& cmd, unsigned long timeout) {
  esp.print(cmd);
  esp.print('\n');
  return espWaitFor("OK", timeout);
}

// Ставим скорость UART 9600 на ESP-01
void espSetBaud() {
  esp.begin(9600);
  delay(300);
  esp.flush();
  esp.print("AT\n");
  if (!espWaitFor("OK", 1500)) {          // возможно ESP на 115200
    esp.end();
    esp.begin(115200);
    delay(200);
    esp.print("AT+UART_DEF=9600,8,1,0,0\n");
    espWaitFor("OK", 1500);
    esp.end();
    esp.begin(9600);
    delay(200);
  }
}

void espInit() {
  espSetBaud();

  if (WIFI_MODE_AP) {
    espCmd(F("AT+CWMODE=2\n"), 3000);     // точка доступа
    String ap = F("AT+CWSAP=\""); ap += WIFI_AP_SSID; ap += F("\",\""); ap += WIFI_AP_PASS; ap += F("\",5,3\n");
    espCmdStr(ap, 6000);
  } else {
    espCmd(F("AT+CWMODE=1\n"), 3000);     // станция
    String cj = F("AT+CWJAP=\""); cj += WIFI_STA_SSID; cj += F("\",\""); cj += WIFI_STA_PASS; cj += F("\"\n");
    espCmdStr(cj, 15000);
  }

  espCmd(F("AT+CIPMUX=1\n"), 2000);       // режим сервера на 1–4 TCP
  espCmd(F("AT+CIPSERVER=1,80\n"), 3000); // TCP-сервер на порту 80
  espCmd(F("AT+CIPSTO=0\n"), 2000);       // линк не закрывать по таймауту
}

// ------------------------------------------------------------------
// HTTP-парсер ESP-01 (+IPD)
// ------------------------------------------------------------------
enum EspState { ESP_IDLE, ESP_TAG, ESP_PARSE_LINK, ESP_PARSE_LEN, ESP_PAYLOAD, ESP_WAIT_PROMPT };
EspState espState = ESP_IDLE;
const char ESP_TAG[] = "+IPD,";
int espTagIdx = 0;
int espLinkId = 0;
int espRemain = 0;
char espPayload[320];
int  espPlen = 0;
String pendingResp = "";
bool respPending = false;
unsigned long pendingStart = 0;

void espProcessPayload() {
  // Первая строка запроса: "GET /cmd/... HTTP/1.1"
  String req = "";
  for (int i = 0; i < espPlen; i++) {
    if (espPayload[i] == '\r' || espPayload[i] == '\n') break;
    req += espPayload[i];
  }
  if (req.startsWith("GET ")) {
    String path = req.substring(4);
    int sp = path.indexOf(' ');
    if (sp >= 0) path = path.substring(0, sp);
    if (path.startsWith("/cmd/")) {
      String cmd = path.substring(5);
      cmd.replace("%3D", "=");           // подстраховка от url-кодировки '='
      handleCommand(cmd);
    }
  }
  sendStatusBodySafe(espLinkId);
}

void espFeed(char c) {
  switch (espState) {
    case ESP_IDLE:
      if (c == ESP_TAG[0]) { espTagIdx = 1; espState = ESP_TAG; }
      break;

    case ESP_TAG:                          // ищем "+IPD,"
      if (c == ESP_TAG[espTagIdx]) {
        espTagIdx++;
        if (espTagIdx >= 5) { espState = ESP_PARSE_LINK; espLinkId = 0; }
      } else {
        espState = ESP_IDLE;
      }
      break;

    case ESP_PARSE_LINK:                   // "<linkId>,"
      if (c >= '0' && c <= '9') espLinkId = espLinkId * 10 + (c - '0');
      else if (c == ',') { espState = ESP_PARSE_LEN; espRemain = 0; }
      else espState = ESP_IDLE;
      break;

    case ESP_PARSE_LEN:                    // "<len>:"
      if (c >= '0' && c <= '9') espRemain = espRemain * 10 + (c - '0');
      else if (c == ':') { espState = ESP_PAYLOAD; espPlen = 0; }
      else espState = ESP_IDLE;
      break;

    case ESP_PAYLOAD:
      if (espPlen < 319) espPayload[espPlen++] = c;
      espRemain--;
      if (espRemain <= 0) {
        espPayload[espPlen] = 0;
        espProcessPayload();
        if (espState != ESP_WAIT_PROMPT) espState = ESP_IDLE;
      }
      break;

    case ESP_WAIT_PROMPT:                  // ждём '>' от CIPSEND
      if (c == '>') {
        if (respPending) { esp.print(pendingResp); pendingResp = ""; respPending = false; }
        espState = ESP_IDLE;
      }
      break;
  }
}

// ------------------------------------------------------------------
// Команды (общие для Web Serial и HTTP)
// ------------------------------------------------------------------
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

void runScene(String name) {
  if      (name == "home")    { mainLightSet(1); ledSet(75); heatTemp = 24; pumpSet(1); curtainPercent = 0; }
  else if (name == "evening") { mainLightSet(0); ledSet(50); heatTemp = 21; curtainPercent = 100; }
  else if (name == "cinema")  { curtainPercent = 100; ledSet(25); }
  else if (name == "morning") { curtainPercent = 0; ledSet(40); heatTemp = 22; }
  else if (name == "night")   { mainLightSet(0); ledSet(0); heatTemp = 19; curtainPercent = 100; }
  else if (name == "away")    { mainLightSet(0); ledSet(0); heatTemp = 19; pumpSet(0); acPowerSet(0); acFan = 1; curtainPercent = 100; }
  curtains.moveTo((long)curtainPercent * CURTAIN_MAX_STEPS / 100L);
}

// ------------------------------------------------------------------
// Состояние (ответы)
// ------------------------------------------------------------------
void statusInto(char* buf, int maxLen) {
  snprintf(buf, maxLen,
    "L=%d;LED=%d;TEMP=%d;PUMP=%d;AC=%d;ACTEMP=%d;ACMODE=%s;ACFAN=%d;CUR=%d",
    mainLight, ledVal, heatTemp, pumpOn, acOn, acTemp, acMode.c_str(), acFan, curtainPercent);
}

void sendStatus() {
  char b[160];
  statusInto(b, sizeof(b));
  Serial.println(b);
}

void sendStatusBodySafe(byte linkId) {
  char b[160];
  statusInto(b, sizeof(b));
  String body = F("HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
  body += String(b);
  body += "\r\n";
  respPending = body;
  pendingStart = millis();
  espState = ESP_WAIT_PROMPT;
  esp.print(F("AT+CIPSEND="));
  esp.print(linkId);
  esp.print(',');
  esp.println(body.length());
}

// ------------------------------------------------------------------
void setup() {
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

  Serial.begin(9600);
  Serial.println(F("Smart Home WiFi: booting ESP-01..."));
  espInit();
  Serial.println(F("WiFi server ready."));
}

void loop() {
  // --- ESP-01 (HTTP) ---
  while (esp.available()) {
    espFeed(esp.read());
  }
  if (respPending && millis() - pendingStart > 2000) {
    respPending = false;
    espState = ESP_IDLE;
  }

  // --- Web Serial / отладочная консоль (опционально) ---
  static String serialLine = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      serialLine.trim();
      if (serialLine.length() > 0) handleCommand(serialLine);
      serialLine = "";
    } else {
      serialLine += c;
      if (serialLine.length() > 120) serialLine = "";
    }
  }

  // --- Шторы ---
  curtains.moveTo((long)curtainPercent * CURTAIN_MAX_STEPS / 100L);
  if (curtains.distanceToGo() != 0) curtains.run();

  // --- Датчик температуры (опция) ---
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