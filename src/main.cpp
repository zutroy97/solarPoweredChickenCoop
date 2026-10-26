#include <Arduino.h>
/*
  Coop water and air temperature -> RAK4631 (Meshtastic TEXTMSG) -> Home Assistant

  Board:     Raspberry Pi Pico (RP2040) - PlatformIO env "pico" (default)
             Arduino core: "Raspberry Pi Pico/RP2040/RP2350" by Earle Philhower
  Libraries: OneWire, DallasTemperature, Adafruit SH110X, Adafruit GFX (Library Manager)

  Wiring
    Pico GP0 (Serial1 TX) -> RAK13002 RXD1
    Pico GP1 (Serial1 RX) <- RAK13002 TXD1
    Pico GND              -> RAK GND (common ground)
    Pico GP10 (pin 14)    -> DS18B20 data (yellow or white wire)
    4.7k resistor         between GP10 and 3V3 (one resistor for all probes)
    DS18B20 red -> 3V3, black -> GND
    Pico GP4 (pin 6)      -> OLED SDA   (1.3" SH1106 128x64, I2C address 0x3C)
    Pico GP5 (pin 7)      -> OLED SCL
    OLED VCC -> 3V3, GND -> GND (the module has its own I2C pull-ups)

  RAK Meshtastic serial module
    Enabled, mode TEXTMSG, 38400 baud, RX pin 15, TX pin 16, GPS mode Not_Present

  Messages sent (one line, broadcast on the Coop channel):
    {"fr":"coop1","t":"env","wt":3.4,"at":-5.1,"r":"timer"}
      wt = water temperature C, at = air temperature C (null if a probe is missing)
      r  = reason: boot, timer, alert, req
    The boot report also carries rs = why the Pico last reset:
      power, run_pin, soft, wdt, debug, glitch, brownout, unknown

  Commands accepted (sent from Home Assistant):
    {"to":"coop1","c":"status"}   -> sends a reading immediately
    {"to":"coop1","c":"reboot"}   -> restarts the Pico (boot report follows with rs "soft")
  On the USB serial monitor, Enter sends a reading and typing reboot restarts the Pico.

  A hardware watchdog restarts the Pico if loop() stalls for WDT_MS.

  The OLED wakes for SCREEN_ON_MS at boot and after every report, then blanks
  to save solar power and avoid burn-in. Without an OLED the node runs as before.
*/

#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

// ---------------- settings ----------------
const char* NODE_ID         = "coop1";
const uint8_t ONE_WIRE_PIN  = 10;
const unsigned long REPORT_MS    = 10UL * 60UL * 1000UL;  // regular report every 10 min
const unsigned long CHECK_MS     = 60UL * 1000UL;         // local check every minute
const unsigned long MIN_ALERT_MS = 5UL * 60UL * 1000UL;   // at most one alert report per 5 min
const float ALERT_WATER_C   = 2.0;                        // report immediately when crossing this
const uint32_t WDT_MS       = 8000;                       // watchdog timeout (RP2040 max is about 8.3 s)
const uint8_t I2C_SDA_PIN   = 4;
const uint8_t I2C_SCL_PIN   = 5;
const uint8_t OLED_ADDR     = 0x3C;
const unsigned long SCREEN_ON_MS = 60UL * 1000UL;         // OLED stays on this long after a report

// After the first boot, copy the probe addresses printed on the USB serial
// monitor into these two lines. Left as all zeros, the first probe found is
// used as "water" and the second as "air".
DeviceAddress WATER_ADDR = {0x28, 0xFF, 0xB6, 0xAB, 0x03, 0x15, 0x02, 0xED};
DeviceAddress AIR_ADDR = {0x28, 0xFF, 0x4B, 0x77, 0x03, 0x15, 0x02, 0xB4};  // warmed-in-hand probe
// -------------------------------------------

OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature sensors(&oneWire);
Adafruit_SH1106G display(128, 64, &Wire, -1);  // 1.3" module; an SSD1306 driver shows garbage on it
bool haveScreen = false;
bool screenOn = false;
unsigned long screenOnAt = 0;

DeviceAddress found[4];
uint8_t foundCount = 0;
const uint8_t* waterAddr = nullptr;
const uint8_t* airAddr = nullptr;

unsigned long lastReport = 0;
unsigned long lastCheck = 0;
unsigned long lastAlert = 0;
bool waterWasLow = false;
String rxLine;
const char* resetReason = "unknown";

const char* resetReasonName() {
  switch (rp2040.getResetReason()) {
    case RP2040::PWRON_RESET:    return "power";
    case RP2040::RUN_PIN_RESET:  return "run_pin";
    case RP2040::SOFT_RESET:     return "soft";
    case RP2040::WDT_RESET:      return "wdt";
    case RP2040::DEBUG_RESET:    return "debug";
    case RP2040::GLITCH_RESET:   return "glitch";
    case RP2040::BROWNOUT_RESET: return "brownout";
    default:                     return "unknown";
  }
}

bool isZero(const uint8_t* a) {
  for (int i = 0; i < 8; i++) if (a[i]) return false;
  return true;
}

void printAddr(const uint8_t* a) {
  Serial.print("{");
  for (int i = 0; i < 8; i++) {
    Serial.print("0x");
    if (a[i] < 16) Serial.print('0');
    Serial.print(a[i], HEX);
    if (i < 7) Serial.print(", ");
  }
  Serial.print("}");
}

void findProbes() {
  sensors.begin();
  foundCount = 0;
  uint8_t n = sensors.getDeviceCount();
  for (uint8_t i = 0; i < n && foundCount < 4; i++) {
    if (sensors.getAddress(found[foundCount], i)) foundCount++;
  }
  Serial.printf("Found %d temperature probe(s)\n", foundCount);
  for (uint8_t i = 0; i < foundCount; i++) {
    Serial.printf("  probe %d: ", i);
    printAddr(found[i]);
    Serial.println();
    sensors.setResolution(found[i], 12);
  }

  waterAddr = !isZero(WATER_ADDR) ? WATER_ADDR : (foundCount > 0 ? found[0] : nullptr);
  airAddr   = !isZero(AIR_ADDR)   ? AIR_ADDR   : (foundCount > 1 ? found[1] : nullptr);
  if (isZero(WATER_ADDR) && foundCount > 0) {
    Serial.println("Using discovery order (probe 0 = water, probe 1 = air).");
    Serial.println("Warm one probe in your hand to see which is which, then set WATER_ADDR and AIR_ADDR.");
  }
}

float readC(const uint8_t* addr) {
  if (!addr) return NAN;
  float t = sensors.getTempC(addr);
  if (t == DEVICE_DISCONNECTED_C || t == 85.0f) return NAN;  // 85.0 = power-on value, not a reading
  return t;
}

String fmt(float v) {
  return isnan(v) ? String("null") : String(v, 1);
}

void setupScreen() {
  Wire.setSDA(I2C_SDA_PIN);
  Wire.setSCL(I2C_SCL_PIN);
  Wire.begin();

  // List everything on the bus; use 0x3D if the module answers there instead of 0x3C
  uint8_t oledAddr = 0;
  Serial.print("I2C devices on GP4/GP5:");
  for (uint8_t a = 0x08; a < 0x78; a++) {
    rp2040.wdt_reset();  // a stuck bus can make each probe slow
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf(" 0x%02X", a);
      if (a == OLED_ADDR || (a == 0x3D && !oledAddr)) oledAddr = a;
    }
  }
  Serial.println();

  if (!oledAddr || !display.begin(oledAddr, true)) {
    Serial.println("No OLED found at 0x3C or 0x3D - continuing without a screen");
    return;
  }
  haveScreen = true;
  display.cp437(true);  // for the degree sign (char 248)
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(NODE_ID);
  display.println("starting...");
  display.display();
  screenOn = true;
  screenOnAt = millis();
  Serial.printf("OLED ready at 0x%02X\n", oledAddr);
}

// One temperature row: label, big Celsius value, small Fahrenheit value
void drawTemp(int16_t y, const char* label, float c) {
  display.setTextSize(1);
  display.setCursor(0, y + 4);
  display.print(label);
  display.setTextSize(2);
  display.setCursor(32, y);
  if (isnan(c)) {
    display.print("--");
    return;
  }
  display.print(String(c, 1));
  display.write((char)248);
  display.print("C");
  // Fahrenheit in small text after the Celsius value, only if it fits (e.g. not at -12.5 C)
  String f = String(c * 9.0f / 5.0f + 32.0f, 0) + "F";
  int16_t x = display.getCursorX() + 2;
  if (x + 6 * (int16_t)f.length() <= 128) {
    display.setTextSize(1);
    display.setCursor(x, y + 4);
    display.print(f);
  }
}

void showOnScreen(const char* reason, float wt, float at) {
  if (!haveScreen) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(NODE_ID);
  display.print("  sent: ");
  display.print(reason);
  drawTemp(14, "Water", wt);
  drawTemp(34, "Air", at);
  unsigned long m = millis() / 60000UL;
  display.setTextSize(1);
  display.setCursor(0, 56);
  display.printf("up %luh%02lum", m / 60, m % 60);
  if (strcmp(reason, "boot") == 0) {
    display.print(" rs ");
    display.print(resetReason);
  }
  display.display();
  if (!screenOn) display.oled_command(SH110X_DISPLAYON);
  screenOn = true;
  screenOnAt = millis();
}

void screenTimeout() {
  if (screenOn && millis() - screenOnAt >= SCREEN_ON_MS) {
    display.oled_command(SH110X_DISPLAYOFF);
    screenOn = false;
  }
}

void sendReport(const char* reason, float wt, float at) {
  String msg = String("{\"fr\":\"") + NODE_ID + "\",\"t\":\"env\",\"wt\":" + fmt(wt) +
               ",\"at\":" + fmt(at) + ",\"r\":\"" + reason + "\"";
  if (strcmp(reason, "boot") == 0) msg += String(",\"rs\":\"") + resetReason + "\"";
  msg += "}";
  Serial1.println(msg);  // RAK broadcasts this line on the Coop channel
  Serial.print("TX ");
  Serial.println(msg);
  showOnScreen(reason, wt, at);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(100);
  digitalWrite(LED_BUILTIN, LOW);
  lastReport = millis();
}

void readAndMaybeReport(const char* reason, bool force) {
  sensors.requestTemperatures();  // about 750 ms at 12-bit resolution
  float wt = readC(waterAddr);
  float at = readC(airAddr);
  Serial.printf("[%lus] water %s C, air %s C\n", millis() / 1000, fmt(wt).c_str(), fmt(at).c_str());

  bool low = !isnan(wt) && wt < ALERT_WATER_C;
  bool crossed = (low != waterWasLow);
  waterWasLow = low;

  if (force) {
    sendReport(reason, wt, at);
  } else if (crossed && millis() - lastAlert >= MIN_ALERT_MS) {
    lastAlert = millis();
    sendReport("alert", wt, at);
  }
}

void rebootNow() {
  Serial.println("Reboot requested");
  Serial.flush();
  delay(100);
  rp2040.reboot();
}

void handleLine(const String& line) {
  Serial.print("RX ");
  Serial.println(line);
  // TEXTMSG prints received messages as "SHORTNAME: text"
  bool forMe = line.indexOf(String("\"to\":\"") + NODE_ID + "\"") >= 0 ||
               line.indexOf("\"to\":\"all\"") >= 0;
  if (forMe && line.indexOf("\"c\":\"status\"") >= 0) {
    readAndMaybeReport("req", true);
  } else if (forMe && line.indexOf("\"c\":\"reboot\"") >= 0) {
    rebootNow();
  }
}

void setup() {
  resetReason = resetReasonName();  // read before anything else touches the watchdog
  Serial.begin(115200);
  Serial1.setTX(0);
  Serial1.setRX(1);
  Serial1.begin(38400);
  pinMode(LED_BUILTIN, OUTPUT);

  // Wait up to 10 s for the USB serial monitor so the startup lines are visible.
  // Without a monitor (at the coop) it simply continues after 10 s.
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 10000) delay(10);
  delay(500);
  Serial.println("\nCoop temperature node starting");
  Serial.println("Press Enter in the serial monitor for a reading.");
  Serial.printf("Last reset: %s\n", resetReason);
  rp2040.wdt_begin(WDT_MS);  // started after the USB wait above, which can take 10 s
  setupScreen();
  findProbes();
  readAndMaybeReport("boot", true);
  lastCheck = millis();
}

void loop() {
  rp2040.wdt_reset();
  screenTimeout();

  // USB serial monitor: "reboot" restarts, anything else (just Enter) sends a reading now
  if (Serial.available()) {
    delay(20);  // let the rest of the typed line arrive
    String usbLine;
    while (Serial.available()) usbLine += (char)Serial.read();
    if (usbLine.indexOf("reboot") >= 0) {
      rebootNow();
    }
    readAndMaybeReport("req", true);
  }

  // Read lines arriving from the RAK
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n') {
      rxLine.trim();
      if (rxLine.length()) handleLine(rxLine);
      rxLine = "";
    } else if (c != '\r' && rxLine.length() < 240) {
      rxLine += c;
    }
  }

  unsigned long now = millis();
  if (now - lastReport >= REPORT_MS) {
    readAndMaybeReport("timer", true);
    lastCheck = now;
  } else if (now - lastCheck >= CHECK_MS) {
    readAndMaybeReport("check", false);
    lastCheck = now;
  }
}
