/*
 * ================================================================
 *  NEXALWARE — Smart Socket Firmware  v3.0  (ESP32 Production)
 *  Author : Adeyemo Dare Timileyin
 * ================================================================
 *
 *  HARDWARE:
 *    Board   : ESP32-WROOM-32D (ESP32 Dev Module)
 *    Relay   : GPIO 32  (Active LOW — relay ON when GPIO LOW)
 *    LCD     : 16×4 parallel 4-bit
 *              RS=13  E=12  D4=14  D5=27  D6=26  D7=25  RW→GND
 *    RTC     : DS1307  SDA=GPIO21  SCL=GPIO22
 *    LED     : Red  (Power)  GPIO 15 — always ON at boot
 *              Green (WiFi)   GPIO 4  — ON when WiFi connected
 *              Yellow(Cloud)  GPIO 5  — ON when MQTT connected
 *              Blue   (Relay)  GPIO 18 — mirrors relay state
 *
 *  NEXALWARE DEVICE CREDENTIALS:
 *    Device ID : ""
 *    MQTT Host : ""
 *    MQTT Port : 1883
 *    Username  : ""
 *    Password  : ""
 *
 *  WIFI:
 *    SSID     : ""
 *    Password : ""
 *
 *  LIBRARIES REQUIRED:
 *    - PubSubClient  v2.8   (Nick O'Leary)
 *    - ArduinoJson   v6.x   (Benoit Blanchon)
 *    - LiquidCrystal v1.0.7 (Arduino)
 *    - RTClib        v2.x   (Adafruit)
 *    - WiFi.h, EEPROM.h, Wire.h (ESP32 Arduino core v3.3.10)
 * ================================================================
 */

// ── LIBRARIES ───────────────────────────────────────────────────
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal.h>
#include <RTClib.h>
#include <EEPROM.h>

// ── WIFI CREDENTIALS ────────────────────────────────────────────
const char* WIFI_SSID     = "your wifi ssid";
const char* WIFI_PASSWORD = "your wifi password";

// ── NEXALWARE DEVICE CREDENTIALS ────────────────────────────────
const char* MQTT_HOST   = "mqtt.nexalware.com";
const int   MQTT_PORT   = 1883;
const char* MQTT_USER   = "your mqtt username";
const char* MQTT_PASS   = "your mqtt password";
const char* DEVICE_ID   = "your device id";

// MQTT topics — built in setup() from DEVICE_ID
char TOPIC_CMD[80];
char TOPIC_STATUS[80];
char TOPIC_SCHED[80];

// ── PIN DEFINITIONS ──────────────────────────────────────────────
#define RELAY_PIN   19   // Relay control — Active LOW
#define TZ_OFFSET_SECONDS 3600UL

// Status LEDs
#define LED_RED     18  // Relay state  
#define LED_GREEN    4  // WiFi connected
#define LED_YELLOW   5  // MQTT/Cloud connected
#define LED_BLUE     15  // Power indicator   — always ON

// ── LCD (16 cols × 4 rows, parallel 4-bit) ──────────────────────
LiquidCrystal lcd(13, 12, 14, 27, 26, 25);

// ── RTC ──────────────────────────────────────────────────────────
RTC_DS1307 rtc;

// ── MQTT / WIFI CLIENTS ──────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ── SCHEDULE SLOTS ───────────────────────────────────────────────
#define MAX_SLOTS 5

struct Slot {
  uint32_t onTs;       // Unix epoch — relay ON  (0 = OFF-only schedule)
  uint32_t offTs;      // Unix epoch — relay OFF (must be > 0)
  uint8_t  enabled;    // 1 = active, 0 = empty
  uint8_t  override;   // 1 = user manually turned OFF during schedule window
  char     label[8];   // optional 7-char label + null terminator
};

Slot slots[MAX_SLOTS];

// ── STATE ─────────────────────────────────────────────────────────
bool relayON = false;
bool mqttOK  = false;

// ── EEPROM MAP ────────────────────────────────────────────────────
// Byte 0   : relay state (0 or 1)
// Byte 1   : signature  (0xA5 = initialised)
// Byte 4+  : slots array
#define ADDR_RELAY  0
#define ADDR_SIG    1
#define ADDR_SLOTS  4
#define SIG_VALUE   0xA5
#define EEPROM_SIZE 512

// ── NON-BLOCKING TIMING ───────────────────────────────────────────
unsigned long tLCD    = 0;
unsigned long tStatus = 0;
unsigned long tSched  = 0;

#define INT_LCD    1000    // LCD refresh every 1 second
#define INT_STATUS 10000   // Publish status every 10 seconds
#define INT_SCHED  15000   // Check schedules every 15 seconds

// ── DAY NAMES IN FLASH ────────────────────────────────────────────
const char _D0[] PROGMEM = "Sun"; const char _D1[] PROGMEM = "Mon";
const char _D2[] PROGMEM = "Tue"; const char _D3[] PROGMEM = "Wed";
const char _D4[] PROGMEM = "Thu"; const char _D5[] PROGMEM = "Fri";
const char _D6[] PROGMEM = "Sat";
const char* const DAYS[] PROGMEM = {_D0,_D1,_D2,_D3,_D4,_D5,_D6};


// ================================================================
//  LED HELPERS
// ================================================================
void ledInit() {
  pinMode(LED_BLUE,   OUTPUT);
  pinMode(LED_GREEN,  OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_RED,    OUTPUT);
  digitalWrite(LED_BLUE,   LOW); // power LED on immediately
  digitalWrite(LED_GREEN,  LOW);
  digitalWrite(LED_YELLOW, LOW);
  digitalWrite(LED_RED,    HIGH);
}

void ledWifi(bool on)  { digitalWrite(LED_GREEN,  on ? HIGH : LOW); }
void ledCloud(bool on) { digitalWrite(LED_YELLOW, on ? HIGH : LOW); }
void ledRelay(bool on) { digitalWrite(LED_BLUE, on ? HIGH : LOW); }


// ================================================================
//  EEPROM
// ================================================================
void eepromInitFresh() {
  EEPROM.write(ADDR_RELAY, 0);
  EEPROM.write(ADDR_SIG,   SIG_VALUE);
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    Slot blank = {0, 0, 0, 0, {0}};
    EEPROM.put(ADDR_SLOTS + i * sizeof(Slot), blank);
  }
  EEPROM.commit();
  Serial.println(F("EEPROM:INIT"));
}

void eepromLoad() {
  if (EEPROM.read(ADDR_SIG) != SIG_VALUE) {
    eepromInitFresh();
    for (uint8_t i = 0; i < MAX_SLOTS; i++) {
      slots[i] = {0, 0, 0, 0, {0}};
    }
    relayON = false;
    return;
  }
  relayON = (EEPROM.read(ADDR_RELAY) == 1);
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    EEPROM.get(ADDR_SLOTS + i * sizeof(Slot), slots[i]);
    // Sanitise corrupt EEPROM values (0xFFFFFFFF = uninitialised flash)
    if (slots[i].onTs  == 0xFFFFFFFF) slots[i].onTs  = 0;
    if (slots[i].offTs == 0xFFFFFFFF) slots[i].offTs = 0;
    if (slots[i].enabled  > 1) slots[i].enabled  = 0;
    if (slots[i].override > 1) slots[i].override = 0;
    if (!slots[i].offTs)        slots[i].enabled  = 0;
    slots[i].label[7] = '\0';
  }
}

void eepromSaveRelay() {
  EEPROM.write(ADDR_RELAY, relayON ? 1 : 0);
  EEPROM.commit();
}

void eepromSaveSlots() {
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    EEPROM.put(ADDR_SLOTS + i * sizeof(Slot), slots[i]);
  }
  EEPROM.commit();
}


// ================================================================
//  HELPERS
// ================================================================
uint32_t rtcNow() {
  if (!rtc.isrunning()) return 0;
  return rtc.now().unixtime() - TZ_OFFSET_SECONDS;   // convert RTC's local-labeled value to true UTC
}



void fmtTs(uint32_t ts, char* buf) {
  if (!ts) { strcpy(buf, "--/-- --:--"); return; }
  DateTime d(ts + TZ_OFFSET_SECONDS);   // convert true UTC back to local WAT for display
  snprintf(buf, 12, "%02d/%02d %02d:%02d",
           d.day(), d.month(), d.hour(), d.minute());
}


// ================================================================
//  RELAY
// ================================================================
void setRelay(bool on, bool save = true) {
  relayON = on;
  // Relay module is Active LOW — LOW = relay energised (ON)
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  ledRelay(on);
  if (save) eepromSaveRelay();
  Serial.println(on ? F("RELAY:ON") : F("RELAY:OFF"));
}


// ================================================================
//  LCD
// ================================================================
void lcdRow(uint8_t row, const char* text) {
  char buf[17];
  snprintf(buf, 17, "%-16s", text);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void refreshLCD() {
  char buf[17];

  // Row 0 — Day and Date
  if (rtc.isrunning()) {
    DateTime now = rtc.now();
    char day[4];
    strcpy_P(day, (char*)pgm_read_ptr(&(DAYS[now.dayOfTheWeek()])));
    snprintf(buf, 17, "%s %02d/%02d/%04d",
             day, now.day(), now.month(), now.year());
  } else {
    strcpy(buf, "RTC not found   ");
  }
  lcdRow(0, buf);

  // Row 1 — Current time
  if (rtc.isrunning()) {
    DateTime now = rtc.now();
    uint8_t hr = now.hour();
    const char* ampm = "AM";
    if (hr >= 12) { ampm = "PM"; if (hr > 12) hr -= 12; }
    if (hr == 0) hr = 12;
    snprintf(buf, 17, "%02d:%02d:%02d %s",
            hr, now.minute(), now.second(), ampm);
  } else {
    strcpy(buf, "Time: --:--:--  ");
  }
  lcdRow(1, buf);

  // Row 2 — Relay state
  snprintf(buf, 17, "Socket  :  %s", relayON ? "ON " : "OFF");
  lcdRow(2, buf);

  // Row 3 — Next active schedule
  uint8_t found = MAX_SLOTS;
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (slots[i].enabled) { found = i; break; }
  }

  if (found < MAX_SLOTS) {
    uint32_t now = rtcNow();
    bool active = (now > 0 &&
                   now >= slots[found].onTs &&
                   now <  slots[found].offTs);
    char ts[12];
    fmtTs(active ? slots[found].offTs : slots[found].onTs, ts);
    // Format: "1>ON 11/06 08:00" (16 chars exactly)
    snprintf(buf, 17, "%d>%s %s",
             found + 1,
             active ? "OF" : "ON",
             ts);
  } else {
    strcpy(buf, "No schedule     ");
  }
  lcdRow(3, buf);
}

void lcdSplash() {
  lcd.clear();
  lcdRow(0, "  Smart Socket  ");
  lcdRow(1, " IoT Controller ");
  lcdRow(2, "  Telecom. Eng  ");
  lcdRow(3, "  Starting...   ");
  delay(2000);
  lcd.clear();
}


// ================================================================
//  SCHEDULE CHECK
//  Called every INT_SCHED ms from loop()
// ================================================================
void checkSchedules() {
  if (!rtc.isrunning()) return;
  uint32_t now = rtcNow();
  if (now == 0) return;

  bool changed = false;

  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (!slots[i].enabled) continue;

    // Turn ON — only if inside the ON→OFF window, relay is OFF,
    // and user has not manually overridden
    // onTs == 0 means OFF-only schedule — skip the ON trigger
    if (slots[i].onTs > 0 &&
        now >= slots[i].onTs &&
        now <  slots[i].offTs &&
        !relayON &&
        !slots[i].override) {
      Serial.print(F("SCHED:ON slot=")); Serial.println(i);
      setRelay(true);
    }

    // Turn OFF — fires when offTs is reached regardless of
    // how the relay was turned ON (manual or scheduled)
    if (now >= slots[i].offTs && relayON) {
      Serial.print(F("SCHED:OFF slot=")); Serial.println(i);
      setRelay(false);
      slots[i].enabled  = 0;
      slots[i].override = 0;
      changed = true;
    }

    // Expire slot silently if relay is already off past offTs
    if (now >= slots[i].offTs && !relayON && slots[i].enabled) {
      slots[i].enabled  = 0;
      slots[i].override = 0;
      changed = true;
    }
  }

  if (changed) {
    eepromSaveSlots();
    refreshLCD();
  }
}


// ================================================================
//  PUBLISH STATUS — sends device state to Nexalware server
// ================================================================
void publishStatus() {
  if (!mqtt.connected()) return;

  StaticJsonDocument<256> doc;
  doc[F("device_id")] = DEVICE_ID;
  doc[F("relay")]     = relayON ? F("ON") : F("OFF");
  doc[F("uptime")]    = millis() / 1000UL;
  if (rtc.isrunning()) doc[F("ts")] = rtcNow();

  JsonArray arr = doc.createNestedArray(F("schedules"));
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (!slots[i].enabled) continue;
    JsonObject s = arr.createNestedObject();
    s[F("slot")]  = i;
    s[F("onTs")]  = slots[i].onTs;
    s[F("offTs")] = slots[i].offTs;
    s[F("mo")]    = slots[i].override;
    s[F("label")] = slots[i].label;
  }

  char out[256];
  serializeJson(doc, out);
  mqtt.publish(TOPIC_STATUS, out);
}


// ================================================================
//  PUBLISH SLOTS — sends schedule list to Nexalware server
// ================================================================
void publishSlots() {
  if (!mqtt.connected()) return;

  StaticJsonDocument<256> doc;
  doc[F("type")] = F("schedules");
  JsonArray arr = doc.createNestedArray(F("schedules"));
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    JsonObject s = arr.createNestedObject();
    s[F("slot")]    = i;
    s[F("enabled")] = slots[i].enabled;
    s[F("onTs")]    = slots[i].onTs;
    s[F("offTs")]   = slots[i].offTs;
    s[F("mo")]      = slots[i].override;
    s[F("label")]   = slots[i].label;
  }

  char out[256];
  serializeJson(doc, out);
  // Publish to schedule topic so Nexalware bridge receives it
  mqtt.publish(TOPIC_SCHED, out);
}


// ================================================================
//  SAVE / CLEAR SLOT HELPERS
// ================================================================
void saveSlot(uint8_t i, uint32_t onTs, uint32_t offTs,
              const char* label) {
  slots[i].onTs     = onTs;
  slots[i].offTs    = offTs;
  slots[i].enabled  = 1;
  slots[i].override = 0;
  memset(slots[i].label, 0, sizeof(slots[i].label));
  if (label && label[0]) {
    strncpy(slots[i].label, label, 7);
    slots[i].label[7] = '\0';
  }
  eepromSaveSlots();
  refreshLCD();
  Serial.print(F("ACK:SLOT_SET:")); Serial.println(i);

  // Fire immediately if we are already inside the schedule window
  uint32_t now = rtcNow();
  if (onTs == 0) {
    Serial.println(F("INFO:OFF_ONLY_SCHED"));
  } else if (now > 0 && now >= onTs && now < offTs && !relayON) {
    Serial.println(F("SCHED:FIRING_NOW"));
    setRelay(true);
  } else {
    Serial.println(F("SCHED:SAVED"));
  }
}

void clearSlot(uint8_t i) {
  slots[i] = {0, 0, 0, 0, {0}};
  eepromSaveSlots();
  refreshLCD();
  Serial.print(F("ACK:SLOT_CLEARED:")); Serial.println(i);
}


// ================================================================
//  COMMAND HANDLER — processes JSON commands from Nexalware server
// ================================================================
void handleCommand(const char* raw) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, raw);
  if (err) {
    Serial.print(F("ERR:JSON:")); Serial.println(err.c_str());
    return;
  }

  const char* cmd = doc[F("cmd")] | "";

  // ── ON ──────────────────────────────────────────────────────────
  if (!strcmp(cmd, "ON")) {
    // Clear all schedule overrides so schedules resume normally
    for (uint8_t i = 0; i < MAX_SLOTS; i++) slots[i].override = 0;
    eepromSaveSlots();
    setRelay(true);
    publishStatus();
    Serial.println(F("ACK:ON"));

  // ── OFF ─────────────────────────────────────────────────────────
  } else if (!strcmp(cmd, "OFF")) {
    // Mark override on any currently active schedule window
    uint32_t now = rtcNow();
    for (uint8_t i = 0; i < MAX_SLOTS; i++) {
      if (!slots[i].enabled) continue;
      if (now > 0 && now >= slots[i].onTs && now < slots[i].offTs)
        slots[i].override = 1;
    }
    eepromSaveSlots();
    setRelay(false);
    publishStatus();
    Serial.println(F("ACK:OFF"));

  // ── TOGGLE ──────────────────────────────────────────────────────
  } else if (!strcmp(cmd, "TOGGLE")) {
    if (relayON) {
      uint32_t now = rtcNow();
      for (uint8_t i = 0; i < MAX_SLOTS; i++) {
        if (!slots[i].enabled) continue;
        if (now > 0 && now >= slots[i].onTs && now < slots[i].offTs)
          slots[i].override = 1;
      }
      eepromSaveSlots();
      setRelay(false);
    } else {
      for (uint8_t i = 0; i < MAX_SLOTS; i++) slots[i].override = 0;
      eepromSaveSlots();
      setRelay(true);
    }
    publishStatus();

  // ── STATUS ──────────────────────────────────────────────────────
  } else if (!strcmp(cmd, "STATUS")) {
    publishStatus();

  // ── GET_SCHEDULES ───────────────────────────────────────────────
  } else if (!strcmp(cmd, "GET_SCHEDULES")) {
    publishSlots();

  // ── SET_SCHEDULE ────────────────────────────────────────────────
  } else if (!strcmp(cmd, "SET_SCHEDULE")) {
    int8_t   idx   = doc[F("slot")]  | -1;
    uint32_t onTs  = doc[F("onTs")]  | 0UL;
    uint32_t offTs = doc[F("offTs")] | 0UL;
    const char* lbl = doc[F("label")] | "";

    if (idx >= 0 && idx < MAX_SLOTS && offTs > 0 &&
        (onTs == 0 || offTs > onTs)) {
      saveSlot((uint8_t)idx, onTs, offTs, lbl);
      publishStatus();
    } else {
      Serial.println(F("ERR:INVALID_SCHEDULE"));
    }

  // ── CANCEL_SCHEDULE ─────────────────────────────────────────────
  } else if (!strcmp(cmd, "CANCEL_SCHEDULE")) {
    int8_t idx = doc[F("slot")] | -1;
    if (idx >= 0 && idx < MAX_SLOTS) {
      clearSlot((uint8_t)idx);
      publishStatus();
    } else {
      Serial.println(F("ERR:BAD_SLOT"));
    }

  // ── SYNC_SCHEDULES ──────────────────────────────────────────────
  } else if (!strcmp(cmd, "SYNC_SCHEDULES")) {
    JsonArray arr = doc[F("schedules")];
    if (!arr.isNull()) {
      for (JsonObject s : arr) {
        int8_t idx = s[F("slot")] | -1;
        if (idx < 0 || idx >= MAX_SLOTS) continue;
        uint32_t onTs  = s[F("onTs")]  | 0UL;
        uint32_t offTs = s[F("offTs")] | 0UL;
        if (offTs == 0) { clearSlot((uint8_t)idx); continue; }
        slots[idx].onTs     = onTs;
        slots[idx].offTs    = offTs;
        slots[idx].enabled  = s[F("enabled")]  | (uint8_t)0;
        slots[idx].override = s[F("mo")]        | (uint8_t)0;
        const char* lbl = s[F("label")] | "";
        strncpy(slots[idx].label, lbl, 7);
        slots[idx].label[7] = '\0';
      }
      eepromSaveSlots();
      refreshLCD();
      Serial.println(F("ACK:SYNC_DONE"));
      publishStatus();
    }

  } else {
    Serial.print(F("ERR:UNKNOWN_CMD:")); Serial.println(cmd);
  }
}


// ================================================================
//  MQTT CALLBACK — called by PubSubClient when message arrives
// ================================================================
void mqttCallback(char* topic, byte* msg, unsigned int len) {
  char buf[256];
  uint16_t n = (len < 255) ? (uint16_t)len : 255;
  memcpy(buf, msg, n);
  buf[n] = '\0';
  Serial.print(F("MQTT RX [")); Serial.print(topic);
  Serial.print(F("]: ")); Serial.println(buf);
  handleCommand(buf);
}


void wifiConnect() {
  ledWifi(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  lcdRow(0, "Connecting WiFi ");
  lcdRow(1, WIFI_SSID);
  lcdRow(2, "Please wait...  ");
  lcdRow(3, "                ");

  uint8_t attempts = 0;
  char dots[17] = "                ";

  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    // Show animated dots on row 3
    if (attempts < 16) dots[attempts] = '.';
    lcdRow(3, dots);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    ledWifi(true);
    lcdRow(0, "WiFi Connected! ");
    lcdRow(1, WIFI_SSID);
    // Show IP address on row 2
    char ipBuf[17];
    snprintf(ipBuf, 17, "%s", WiFi.localIP().toString().c_str());
    lcdRow(2, ipBuf);
    lcdRow(3, "                ");
    Serial.print(F("WiFi connected. IP: "));
    Serial.println(WiFi.localIP());
    delay(2000); // let user read the IP before LCD refreshes
  } else {
    ledWifi(false);
    lcdRow(0, "WiFi FAILED     ");
    lcdRow(1, "Check SSID and  ");
    lcdRow(2, "password        ");
    lcdRow(3, "Offline mode... ");
    Serial.println(F("WiFi FAILED — running offline"));
    delay(2000);
  }
}


// ================================================================
//  MQTT CONNECT
// ================================================================
void mqttConnect() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512); // larger buffer for SYNC_SCHEDULES payloads
  ledCloud(false);
  mqttOK = false;

  for (uint8_t i = 0; i < 5 && !mqtt.connected(); i++) {
    Serial.print(F("MQTT connecting attempt "));
    Serial.print(i + 1); Serial.print(F("... "));

    if (mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS)) {
      mqttOK = true;
      mqtt.subscribe(TOPIC_CMD);
      mqtt.subscribe(TOPIC_SCHED);
      Serial.println(F("OK"));
      ledCloud(true);   // Yellow LED ON

      // Ask Nexalware server to push current schedules down to device
      mqtt.publish(TOPIC_STATUS, "{\"cmd\":\"GET_SCHEDULES\"}");
    } else {
      Serial.print(F("failed rc="));
      Serial.println(mqtt.state());
      delay(3000);
    }
  }

  if (!mqttOK) {
    Serial.println(F("MQTT unavailable — offline mode"));
    ledCloud(false);
  }
}


// ================================================================
//  MAINTAIN CONNECTION — called every loop iteration
// ================================================================
void maintainConnection() {
  // Reconnect WiFi if dropped
  if (WiFi.status() != WL_CONNECTED) {
    mqttOK = false;
    ledWifi(false);
    ledCloud(false);
    wifiConnect();
  }

  // Reconnect MQTT if dropped
  if (!mqtt.connected()) {
    mqttOK = false;
    ledCloud(false);
    mqttConnect();
  }
}


// ================================================================
//  SETUP
// ================================================================
void setup() {
    // step 2 
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // Active LOW relay — start OFF (HIGH = coil off)
  
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n\n=== NEXALWARE Smart Socket v3.0 ==="));
  Serial.print(F("Device ID: ")); Serial.println(DEVICE_ID);

  // 1. LED init — Blue power LED ON immediately
  ledInit();

  // 3. EEPROM
  EEPROM.begin(EEPROM_SIZE);
  eepromLoad();
  Serial.print(F("Relay restored from EEPROM: "));
  Serial.println(relayON ? F("ON") : F("OFF"));

  // 4. LCD
  lcd.begin(16, 4);
  lcdSplash();

// 5. RTC
  Wire.begin();
  if (!rtc.begin()) {
    Serial.println(F("ERR: RTC not found — check wiring"));
    lcdRow(0, "RTC ERROR!      ");
    lcdRow(1, "Check wiring    ");
    lcdRow(2, "SDA=GPIO21      ");
    lcdRow(3, "SCL=GPIO22      ");
    delay(3000);
  } else {
    // Always set time from compile timestamp on every boot
    // This ensures time is always accurate
    // After confirming time is correct, change to: if (!rtc.isrunning())
    if (!rtc.isrunning()){
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      Serial.println(F("RTC: time set from compile timestamp"));
    }

    DateTime now = rtc.now();
    char buf[32];
    snprintf(buf, 32, "%02d/%02d/%04d %02d:%02d:%02d",
             now.day(), now.month(), now.year(),
             now.hour(), now.minute(), now.second());
    Serial.println(buf);

    // Show time on LCD briefly so you can verify it
    char row0[17], row1[17];
    snprintf(row0, 17, "%02d/%02d/%04d",
             now.day(), now.month(), now.year());
    uint8_t hr = now.hour();
    const char* ap = hr >= 12 ? "PM" : "AM";
    if (hr > 12) hr -= 12;
    if (hr == 0) hr = 12;
    snprintf(row1, 17, "%02d:%02d:%02d %s",
             hr, now.minute(), now.second(), ap);
    lcdRow(0, "Time set:       ");
    lcdRow(1, row0);
    lcdRow(2, row1);
    lcdRow(3, "                ");
    delay(3000); // show for 3 seconds so you can verify
  }

  // 6. Build MQTT topic strings from DEVICE_ID
  snprintf(TOPIC_CMD,    sizeof(TOPIC_CMD),    "nexalware/devices/%s/command",  DEVICE_ID);
  snprintf(TOPIC_STATUS, sizeof(TOPIC_STATUS), "nexalware/devices/%s/status",   DEVICE_ID);
  snprintf(TOPIC_SCHED,  sizeof(TOPIC_SCHED),  "nexalware/devices/%s/schedule", DEVICE_ID);
  Serial.print(F("Topics built for device: ")); Serial.println(DEVICE_ID);

  // 7. Connect to WiFi and MQTT
  wifiConnect();
  if (WiFi.status() == WL_CONNECTED) {
    mqttConnect();
  }

  // 8. Restore relay state from EEPROM and sync LED
  // Active LOW: relay ON → GPIO LOW, relay OFF → GPIO HIGH
  digitalWrite(RELAY_PIN, relayON ? HIGH : LOW);
  ledRelay(relayON);

  refreshLCD();
  Serial.println(F("READY"));
}


// ================================================================
//  LOOP
// ================================================================
void loop() {
  unsigned long now = millis();

  // Keep WiFi and MQTT alive
  maintainConnection();

  // Process incoming MQTT messages
  mqtt.loop();

  // Non-blocking timed tasks
  if (now - tLCD    >= INT_LCD)    { tLCD    = now; refreshLCD();     }
  if (now - tStatus >= INT_STATUS) { tStatus = now; publishStatus();   }
  if (now - tSched  >= INT_SCHED)  { tSched  = now; checkSchedules();  }

  delay(10); // small yield — reduced from 50ms for better MQTT responsiveness
}
