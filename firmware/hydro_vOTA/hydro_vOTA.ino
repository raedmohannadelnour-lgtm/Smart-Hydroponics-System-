// =====================================================================================
//  Smart Hydroponics Controller: ESP32 firmware
//  Passwords and URLs are in secrets.h; pin wiring is in the YOUR SETTINGS block below.
//
//  Schedules run on REAL KSA time (from internet time servers, NTP):
//   - Each schedule has an "anchor": the date/time its cycle started, saved in flash.
//   - After a reboot or power cut, the ESP32 gets the time and works out exactly where
//     both cycles should be, so no time is skipped or shifted.
//   - Manual mode does NOT pause the schedules; they keep running in the background and
//     take control of the relays again when Automatic mode resumes.
//
//  LIBRARIES: esp32 board package (Espressif), PubSubClient (Nick O'Leary).
//  PARTITION: Tools -> Partition Scheme -> "Minimal SPIFFS (1.9MB APP with OTA)" (any scheme
//  "with OTA" works; never "Huge APP / No OTA", or wireless updates are impossible).
//  WIRELESS UPDATES: see the WIRELESS FIRMWARE UPDATES settings below.
//  BOARD: ESP32-WROOM-DA Module (or ESP32 Dev Module). Serial Monitor: 115200 baud.
//
//  FILE LAYOUT (in order)
//    YOUR SETTINGS  - WiFi, HiveMQ login, log URL/key, pin wiring  <- the only part to edit
//    topics + globals
//    logging        - logEvent() -> Serial + MQTT + background upload task (logTask)
//    relays         - setRelay(), setAllLeds(), load/save states in flash (Preferences)
//    tank           - debounced float switch
//    clock          - NTP time, cyclePos() works out each schedule's position
//    schedules      - runSchedules() (both modes), setLightStart/Schedule/PumpSchedule, reset
//    MQTT           - onMqttMessage() handles commands, maintainConnections() never blocks
//    setup() / loop()
//
//  Full guide (wiring, rebuild, troubleshooting, MQTT topics):
//  "Lamba Smart Hydroponics - System Guide".
// =====================================================================================
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <esp_system.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <ArduinoOTA.h>
#include <HTTPUpdate.h>
// =====================================================================================
//  YOUR SETTINGS
// =====================================================================================
// WiFi, MQTT login, log URL/key and the OTA password live in secrets.h (same folder).
// Copy secrets.example.h to secrets.h and fill it in. secrets.h is git-ignored.
#include "secrets.h"

// ---------------- MQTT BROKER (HiveMQ) ----------------
static const int   MQTT_PORT = 8883;
static const char* CLIENT_ID = "esp32-hydroponics";

// ---------------- WIRING ----------------
// Order: Shelf 1 (bottom), Shelf 2, Shelf 3, Shelf 4 (top), Pump.
// Each number is the ESP32 pin for that relay. Paste your corrected line here.
static const uint8_t RELAY_PINS[5] = { 21, 19, 22, 18, 17 };

// true = that relay switches ON when its pin is LOW. The pump relay is the opposite.
static const bool RELAY_ACTIVE_LOW[5] = { true, true, true, true, false };

// Float switch: one wire to GND, the other to this pin (internal pull-up used)
static const uint8_t FLOAT_PIN = 27;
// Verified: the switch is CLOSED when the tank is EMPTY
static const bool CLOSED_MEANS_FULL = false;

// ---------------- WIRELESS FIRMWARE UPDATES (OTA) ----------------
// Firmware version shown on the dashboard and in the log. Change it with every release.
static const char* FW_VERSION = "2026.10.03";
// 1) Local network: Arduino IDE -> Tools -> Port -> "hydro-esp32 at 192.168.x.x", then
//    Upload as usual. The IDE asks for this password.
static const char* OTA_HOSTNAME = "hydro-esp32";
// OTA_PASSWORD is set in secrets.h
// 2) From anywhere: Admin tab -> Firmware update, with the URL of an exported .bin file.
//    Only HTTPS URLs starting with this text are accepted. Set it to your website, e.g.
//    "https://hydro.your-subdomain.workers.dev/firmware/", so nothing else can be loaded.
static const char* OTA_URL_PREFIX = "https://";

// ---------------- CHANNELS ----------------
const char* PREF_KEYS[5]     = { "led1", "led2", "led3", "led4", "pump" };
const char* CHANNEL_NAMES[5] = { "Shelf 1", "Shelf 2", "Shelf 3", "Shelf 4", "Pump" };
const uint8_t PUMP = 4;

// ---------------- TOPICS ----------------
const char* SET_TOPICS[5] = {
  "hydroponics/led1/set", "hydroponics/led2/set", "hydroponics/led3/set",
  "hydroponics/led4/set", "hydroponics/pump/set"
};
const char* STATE_TOPICS[5] = {
  "hydroponics/led1/state", "hydroponics/led2/state", "hydroponics/led3/state",
  "hydroponics/led4/state", "hydroponics/pump/state"
};
const char* STATUS_TOPIC             = "hydroponics/status";
const char* MODE_SET_TOPIC           = "hydroponics/mode/set";
const char* MODE_STATE_TOPIC         = "hydroponics/mode/state";
const char* SCHEDULE_SET_TOPIC       = "hydroponics/schedule/set";        // light on-hours, e.g. "12.5"
const char* SCHEDULE_STATE_TOPIC     = "hydroponics/schedule/state";
const char* LIGHT_START_SET_TOPIC    = "hydroponics/light_start/set";     // "06:00" (KSA)
const char* LIGHT_START_STATE_TOPIC  = "hydroponics/light_start/state";
const char* PUMP_SCHED_SET_TOPIC     = "hydroponics/pump_schedule/set";   // "15,15" minutes
const char* PUMP_SCHED_STATE_TOPIC   = "hydroponics/pump_schedule/state";
const char* CYCLE_RESET_TOPIC        = "hydroponics/cycle/reset";         // Day 1 starts now
const char* PLANTED_STATE_TOPIC      = "hydroponics/planted/state";       // epoch seconds of Day 1
const char* MANUAL_REMAIN_TOPIC      = "hydroponics/manual_remaining";
const char* CYCLE_STATUS_TOPIC       = "hydroponics/cycle_status";        // "on|off,elapsed,total"
const char* PUMP_CYCLE_STATUS_TOPIC  = "hydroponics/pump_cycle_status";   // "on|off|blocked,elapsed,total"
const char* TANK_STATE_TOPIC         = "hydroponics/tank/state";
const char* LOG_TOPIC                = "hydroponics/log";
const char* BOOT_TIME_TOPIC          = "hydroponics/uptime/boot";        // epoch secs the ESP32 powered on
const char* CONNECTED_TIME_TOPIC     = "hydroponics/uptime/connected";   // epoch secs of the current connection
const char* CLOCK_STATE_TOPIC        = "hydroponics/clock/state";        // "synced" | "waiting" (retained)
// Master switch: a shelf that is "not in use" stays OFF whatever the schedule or buttons say
const char* ENABLED_SET_TOPICS[4] = {
  "hydroponics/led1/enabled/set", "hydroponics/led2/enabled/set",
  "hydroponics/led3/enabled/set", "hydroponics/led4/enabled/set"
};
const char* ENABLED_STATE_TOPICS[4] = {
  "hydroponics/led1/enabled/state", "hydroponics/led2/enabled/state",
  "hydroponics/led3/enabled/state", "hydroponics/led4/enabled/state"
};
const char* ENABLED_PREF_KEYS[4] = { "en1", "en2", "en3", "en4" };
// Pump maintenance switch (Master tab): "on" = held ON until switched off (ignores the
// tank level, e.g. to empty the tank), "off" = held OFF, "none" = pump follows schedule/manual.
const char* PUMP_OVERRIDE_SET_TOPIC   = "hydroponics/pump/override/set";
const char* PUMP_OVERRIDE_STATE_TOPIC = "hydroponics/pump/override/state";   // retained
const char* OTA_SET_TOPIC             = "hydroponics/ota/set";        // URL of a .bin to install
const char* OTA_STATE_TOPIC           = "hydroponics/ota/state";      // progress / result text
const char* FIRMWARE_STATE_TOPIC      = "hydroponics/firmware/state"; // FW_VERSION, retained
// Note: the master switch only governs AUTOMATIC mode. Manual (demo) mode can still switch
// a not-in-use shelf on; when manual mode ends, the schedule turns it back off.

bool relayState[5] = { false, false, false, false, false };
bool shelfEnabled[4] = { true, true, true, true };   // master switch per shelf (saved in flash)

// ---------------- CLOCK ----------------
const long   KSA_OFFSET_SEC = 3L * 3600L;     // Saudi Arabia: UTC+3, no daylight saving
const time_t VALID_TIME     = 1700000000;     // earlier than this = clock not synced yet
const uint32_t DAY_SEC      = 86400UL;
bool clockValid = false;
const char* timeSource = "internet time";   // how the clock was set (for the log)
const char* NTP_1 = "time.google.com";
const char* NTP_2 = "pool.ntp.org";
const char* NTP_3 = "time.cloudflare.com";

// ---------------- SCHEDULE SETTINGS (saved in flash) ----------------
const float MIN_ON_HOURS = 8.0;
const float MAX_ON_HOURS = 16.0;
const int   MIN_PUMP_MIN = 5;
const int   MAX_PUMP_MIN = 60;
bool     autoMode    = true;
float    onHours     = 8.0;
int      pumpOnMin   = 15;
int      pumpOffMin  = 15;
uint32_t lightAnchor = 0;   // epoch seconds: a moment a light ON period started
uint32_t pumpAnchor  = 0;   // epoch seconds: a moment a pump ON period started
uint32_t plantedAt   = 0;   // epoch seconds: "Day 1" (set by the Reset button)

const unsigned long MANUAL_TIMEOUT_MS = 10UL * 60UL * 1000UL;
unsigned long manualStartMs = 0;

// ---------------- TANK ----------------
// The float bobs as the water moves, so single readings flicker. The level is sampled
// every TANK_SAMPLE_MS over a TANK_WINDOW_S-second window, and only changes when at least
// TANK_AGREE_PCT % of that window agrees. After a change it's held for TANK_MIN_HOLD_S so
// the pump can't be switched on and off rapidly.
const unsigned long TANK_SAMPLE_MS  = 200;
const int           TANK_WINDOW_S   = 10;
const int           TANK_AGREE_PCT  = 80;
const unsigned long TANK_MIN_HOLD_S = 30;
const int TANK_SAMPLES = (TANK_WINDOW_S * 1000) / TANK_SAMPLE_MS;   // 50
bool tankFull = true;
bool tankSamples[TANK_SAMPLES];        // true = sample read "full"
int  tankSampleIdx = 0;
unsigned long tankLastSampleMs = 0;
unsigned long tankLastChangeMs = 0;

enum PumpOverride { PUMP_AUTO = 0, PUMP_FORCE_ON = 1, PUMP_FORCE_OFF = 2 };
uint8_t pumpOverride = PUMP_AUTO;       // saved in flash

// ---------------- CONNECTIVITY ----------------
const unsigned long WIFI_RETRY_MS = 15000;
const unsigned long MQTT_RETRY_MS = 5000;
bool mqttWasConnected = false;
unsigned long mqttLostAt = 0;
unsigned long mqttConnectedAtMs = 0;         // millis() when the current MQTT connection started

// Reconnection escalation. The ESP32 is never rebooted for being offline: rebooting would
// lose the clock, and without internet it couldn't get it back, so the schedules would stop.
const unsigned long WIFI_FULL_RESET_MS  = 2UL * 60UL * 1000UL;   // WiFi down this long -> full WiFi restart
const unsigned long MQTT_STALE_RESET_MS = 5UL * 60UL * 1000UL;   // WiFi "up" but broker unreachable this long -> full WiFi restart
bool offlineSawWifiDrop = false;   // during the current outage, did WiFi itself drop?
int  lastMqttRc = 0;               // last broker connect error (PubSubClient state)
int  wifiResets = 0;               // full WiFi restarts during the current outage
long lastRssi = 0;                 // last known WiFi signal strength (dBm)

Preferences preferences;
WiFiClientSecure wifiClient;
PubSubClient mqtt(wifiClient);

// ---------------- LOG QUEUE ----------------
const size_t LOG_MSG_LEN   = 128;
const int    LOG_QUEUE_LEN = 20;
QueueHandle_t logQueue = nullptr;

// =====================================================================================
//  Logging
// =====================================================================================
inline uint8_t relayLevel(uint8_t index, bool on)
{
  return (on == RELAY_ACTIVE_LOW[index]) ? LOW : HIGH;
}

bool cloudLoggingEnabled()
{
  return strstr(LOG_API_URL, "YOUR-SUBDOMAIN") == nullptr;
}

// Records one event: Serial, live MQTT topic, and the cloud log queue. Main loop only.
void logEvent(const char* fmt, ...)
{
  char msg[LOG_MSG_LEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  Serial.print("[log] ");
  Serial.println(msg);
  if (mqtt.connected()) mqtt.publish(LOG_TOPIC, msg);
  if (logQueue) xQueueSend(logQueue, msg, 0);
}

String jsonEscape(const char* s)
{
  String out;
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') out += '\\';
    if ((uint8_t)*s >= 0x20) out += *s;
  }
  return out;
}

int postLog(const char* msg)
{
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, LOG_API_URL)) return -1;
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Log-Key", LOG_API_KEY);
  int code = http.POST("{\"msg\":\"" + jsonEscape(msg) + "\"}");
  http.end();
  return code;
}

void logTask(void*)
{
  char msg[LOG_MSG_LEN];
  for (;;) {
    if (xQueueReceive(logQueue, msg, portMAX_DELAY) != pdTRUE) continue;
    for (int attempt = 0; attempt < 5; attempt++) {
      while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(2000));
      int code = postLog(msg);
      if (code == 200) break;
      Serial.printf("[log] upload failed (HTTP %d)\n", code);
      if (code == 400 || code == 401) break;
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }
}

const char* resetReasonText()
{
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_EXT:      return "reset pin";
    default:               return "other";
  }
}

// =====================================================================================
//  Relays
// =====================================================================================
void setRelay(uint8_t index, bool on, bool publishState = true, bool logIt = true)
{
  bool changed = relayState[index] != on;
  relayState[index] = on;
  digitalWrite(RELAY_PINS[index], relayLevel(index, on));
  if (changed) preferences.putBool(PREF_KEYS[index], on);

  Serial.print(CHANNEL_NAMES[index]);
  Serial.print(": ");
  Serial.println(on ? "ON" : "OFF");

  if (publishState) mqtt.publish(STATE_TOPICS[index], on ? "1" : "0", true);
  if (changed && logIt) logEvent("%s turned %s", CHANNEL_NAMES[index], on ? "on" : "off");
}

// Switches the shelves together for the schedule. Shelves that are not in use stay OFF.
void setAllLeds(bool on, const char* reason)
{
  bool anyChanged = false;
  for (uint8_t i = 0; i < 4; i++) {
    bool want = on && shelfEnabled[i];
    if (relayState[i] != want) anyChanged = true;
    setRelay(i, want, true, false);
  }
  if (anyChanged) logEvent("Lights %s (%s)", on ? "on" : "off", reason);
}

void publishEnabled(uint8_t i)
{
  mqtt.publish(ENABLED_STATE_TOPICS[i], shelfEnabled[i] ? "1" : "0", true);
}

// Master switch for one shelf
void setShelfEnabled(uint8_t i, bool en)
{
  bool changed = shelfEnabled[i] != en;
  shelfEnabled[i] = en;
  if (changed) preferences.putBool(ENABLED_PREF_KEYS[i], en);
  if (!en && relayState[i]) setRelay(i, false, true, false);   // switch it off straight away
  publishEnabled(i);
  if (changed) logEvent(en ? "%s back in use" : "%s taken out of use (kept off)", CHANNEL_NAMES[i]);
  // Back in use: in automatic mode the schedule switches it on at its next check if due
}

void loadStates()
{
  for (uint8_t i = 0; i < 5; i++) relayState[i] = preferences.getBool(PREF_KEYS[i], false);
  for (uint8_t i = 0; i < 4; i++) {
    shelfEnabled[i] = preferences.getBool(ENABLED_PREF_KEYS[i], true);
    if (!shelfEnabled[i]) relayState[i] = false;              // never power a shelf that's not in use
  }
  autoMode    = preferences.getBool("autoMode", true);
  onHours     = constrain(preferences.getFloat("onHours", 8.0), MIN_ON_HOURS, MAX_ON_HOURS);
  pumpOnMin   = constrain(preferences.getInt("pumpOnMin", 15),  MIN_PUMP_MIN, MAX_PUMP_MIN);
  pumpOffMin  = constrain(preferences.getInt("pumpOffMin", 15), MIN_PUMP_MIN, MAX_PUMP_MIN);
  pumpOverride = preferences.getUChar("pumpOvr", PUMP_AUTO);
  if (pumpOverride > PUMP_FORCE_OFF) pumpOverride = PUMP_AUTO;
  if (pumpOverride == PUMP_FORCE_ON)  relayState[PUMP] = true;
  if (pumpOverride == PUMP_FORCE_OFF) relayState[PUMP] = false;
  lightAnchor = preferences.getUInt("lightAnchor", 0);
  pumpAnchor  = preferences.getUInt("pumpAnchor", 0);
  plantedAt   = preferences.getUInt("plantedAt", 0);
}

void applyLoadedStates()
{
  for (uint8_t i = 0; i < 5; i++) digitalWrite(RELAY_PINS[i], relayLevel(i, relayState[i]));
}

// =====================================================================================
//  Tank float switch
// =====================================================================================
bool readTankFull()
{
  bool closed = (digitalRead(FLOAT_PIN) == LOW);
  return CLOSED_MEANS_FULL ? closed : !closed;
}

void publishTankState()
{
  mqtt.publish(TANK_STATE_TOPIC, tankFull ? "full" : "empty", true);
}

void updateTankSensor()
{
  if (millis() - tankLastSampleMs < TANK_SAMPLE_MS) return;
  tankLastSampleMs = millis();
  tankSamples[tankSampleIdx] = readTankFull();
  tankSampleIdx = (tankSampleIdx + 1) % TANK_SAMPLES;

  if (millis() - tankLastChangeMs < TANK_MIN_HOLD_S * 1000UL) return;   // recently changed: hold

  int fullCount = 0;
  for (int i = 0; i < TANK_SAMPLES; i++) if (tankSamples[i]) fullCount++;
  int needed = (TANK_SAMPLES * TANK_AGREE_PCT) / 100;

  bool newState = tankFull;
  if (tankFull && (TANK_SAMPLES - fullCount) >= needed) newState = false;   // clearly empty
  if (!tankFull && fullCount >= needed) newState = true;                    // clearly full
  if (newState != tankFull) {
    tankFull = newState;
    tankLastChangeMs = millis();
    publishTankState();
    logEvent(tankFull ? "Tank full" : "Tank empty");
  }
}

// Start-up: take a quick majority reading so the first state is sensible
void initTankSensor()
{
  int fullCount = 0;
  for (int i = 0; i < 25; i++) { if (readTankFull()) fullCount++; delay(20); }
  tankFull = fullCount > 12;
  for (int i = 0; i < TANK_SAMPLES; i++) tankSamples[i] = tankFull;
  tankLastSampleMs = tankLastChangeMs = millis();
}

// ---------------- Pump safety + maintenance switch ----------------
void publishPumpOverride()
{
  mqtt.publish(PUMP_OVERRIDE_STATE_TOPIC,
               pumpOverride == PUMP_FORCE_ON ? "on" : pumpOverride == PUMP_FORCE_OFF ? "off" : "none", true);
}

void setPumpOverride(uint8_t ov)
{
  bool changed = ov != pumpOverride;
  pumpOverride = ov;
  if (changed) preferences.putUChar("pumpOvr", ov);
  publishPumpOverride();
  if (!changed) return;
  if (ov == PUMP_FORCE_ON)       logEvent("Pump held ON by maintenance switch (runs until switched off)");
  else if (ov == PUMP_FORCE_OFF) logEvent("Pump held OFF by maintenance switch");
  else                           logEvent("Pump maintenance switch released - back to schedule");
  // Released: in manual mode start from OFF; in auto the schedule takes over at once
  if (ov == PUMP_AUTO && !autoMode && relayState[PUMP]) setRelay(PUMP, false, true, false);
}

// Runs every loop: applies the maintenance switch, and stops the pump if the tank is empty
// (in every mode) unless it is deliberately held ON.
void enforcePump()
{
  if (pumpOverride == PUMP_FORCE_ON) {
    if (!relayState[PUMP]) setRelay(PUMP, true, true, false);
    return;
  }
  if (pumpOverride == PUMP_FORCE_OFF) {
    if (relayState[PUMP]) setRelay(PUMP, false, true, false);
    return;
  }
  if (relayState[PUMP] && !tankFull) {
    setRelay(PUMP, false, true, false);
    logEvent("Pump stopped - tank empty");
  }
}

// =====================================================================================
//  Clock helpers
// =====================================================================================
time_t nowEpoch() { return time(nullptr); }

// (Re)start internet time (NTP). Safe to call again; restarts the time requests.
void startNtp()
{
  configTime(0, 0, NTP_1, NTP_2, NTP_3);
}

// Backup time source: the "Date" header of an HTTPS reply from our own Worker.
// Used only if NTP hasn't worked, e.g. when a network blocks NTP (UDP port 123).
bool syncTimeFromHttp()
{
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, LOG_API_URL)) return false;
  const char* keys[] = { "Date" };
  http.collectHeaders(keys, 1);
  http.setTimeout(8000);
  int code = http.GET();
  String date = http.header("Date");          // e.g. "Wed, 30 Sep 2026 08:22:05 GMT"
  http.end();
  if (code <= 0 || date.length() < 20) return false;

  int d, y, H, M, S;
  char mon[4] = { 0 };
  if (sscanf(date.c_str(), "%*[^,], %d %3s %d %d:%d:%d", &d, mon, &y, &H, &M, &S) != 6) return false;
  static const char* MON = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* p = strstr(MON, mon);
  if (!p || strlen(mon) != 3) return false;
  int m = (p - MON) / 3 + 1;

  // Days since 1970-01-01 (civil calendar)
  int yy = y - (m <= 2);
  int era = (yy >= 0 ? yy : yy - 399) / 400;
  unsigned yoe = (unsigned)(yy - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;

  struct timeval tv;
  tv.tv_sec  = (time_t)(days * 86400LL + H * 3600 + M * 60 + S);
  tv.tv_usec = 0;
  if (tv.tv_sec < VALID_TIME) return false;
  settimeofday(&tv, nullptr);
  return true;
}

// Position (seconds) within a repeating cycle of length `period` that started at `anchor`.
// Works for any past or future anchor, so a power cut never shifts the schedule.
uint32_t cyclePos(uint32_t anchor, uint32_t period)
{
  int64_t d = (int64_t)nowEpoch() - (int64_t)anchor;
  int64_t p = d % (int64_t)period;
  if (p < 0) p += period;
  return (uint32_t)p;
}

uint32_t lightOnSec()  { return (uint32_t)(onHours * 3600.0f + 0.5f); }
uint32_t pumpOnSec()   { return (uint32_t)pumpOnMin * 60UL; }
uint32_t pumpPeriod()  { return (uint32_t)(pumpOnMin + pumpOffMin) * 60UL; }

// 12-hour KSA time for log messages, e.g. "7:07 PM"
const char* fmt12(int h, int m)
{
  static char buf[12];
  snprintf(buf, sizeof(buf), "%d:%02d %s", (h % 12) ? (h % 12) : 12, m, h < 12 ? "AM" : "PM");
  return buf;
}

void ksaClock(uint32_t epoch, int& h, int& m)
{
  uint32_t t = (epoch + KSA_OFFSET_SEC) % DAY_SEC;
  h = t / 3600;
  m = (t % 3600) / 60;
}

// =====================================================================================
//  Publishing
// =====================================================================================
void publishLightStart()
{
  if (!lightAnchor) return;
  int h, m;
  ksaClock(lightAnchor, h, m);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
  mqtt.publish(LIGHT_START_STATE_TOPIC, buf, true);
}

void publishPlanted()
{
  char buf[12];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)plantedAt);
  mqtt.publish(PLANTED_STATE_TOPIC, buf, true);
}

void publishPumpSchedule()
{
  char buf[12];
  snprintf(buf, sizeof(buf), "%d,%d", pumpOnMin, pumpOffMin);
  mqtt.publish(PUMP_SCHED_STATE_TOPIC, buf, true);
}

// Uptime info for the dashboard's log panel: when the ESP32 powered on, and when the
// current connection started. Needs the real clock, so it's (re)sent once time is synced.
void publishUptime()
{
  if (!clockValid || !mqtt.connected()) return;
  uint32_t now = (uint32_t)nowEpoch();
  char buf[12];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)(now - millis() / 1000));
  mqtt.publish(BOOT_TIME_TOPIC, buf, true);
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)(now - (millis() - mqttConnectedAtMs) / 1000));
  mqtt.publish(CONNECTED_TIME_TOPIC, buf, true);
}

void publishAllStates()
{
  for (uint8_t i = 0; i < 5; i++) mqtt.publish(STATE_TOPICS[i], relayState[i] ? "1" : "0", true);
  mqtt.publish(MODE_STATE_TOPIC, autoMode ? "auto" : "manual", true);
  char buf[8];
  dtostrf(onHours, 0, 1, buf);
  mqtt.publish(SCHEDULE_STATE_TOPIC, buf, true);
  publishPumpSchedule();
  publishLightStart();
  publishPlanted();
  publishTankState();
  for (uint8_t i = 0; i < 4; i++) publishEnabled(i);
  publishPumpOverride();
}

// =====================================================================================
//  Mode + schedule changes
// =====================================================================================
void setMode(bool toAuto)
{
  if (toAuto != autoMode) {
    autoMode = toAuto;
    if (!toAuto) manualStartMs = millis();
    preferences.putBool("autoMode", autoMode);
    // The schedules never pause. Returning to automatic simply hands the relays back
    // to whatever the schedules say right now.
    logEvent(toAuto ? "Switched to automatic mode" : "Switched to manual mode (10 min)");
  }
  mqtt.publish(MODE_STATE_TOPIC, autoMode ? "auto" : "manual", true);
  for (uint8_t i = 0; i < 5; i++) mqtt.publish(STATE_TOPICS[i], relayState[i] ? "1" : "0", true);
}

// Light hours changed. The lights-on time stays the same; only the off time moves.
void setSchedule(float newOnHours)
{
  newOnHours = constrain(newOnHours, MIN_ON_HOURS, MAX_ON_HOURS);
  bool changed = newOnHours != onHours;
  onHours = newOnHours;
  preferences.putFloat("onHours", onHours);

  char onBuf[8], offBuf[8];
  dtostrf(onHours, 0, 1, onBuf);
  dtostrf(24.0 - onHours, 0, 1, offBuf);
  mqtt.publish(SCHEDULE_STATE_TOPIC, onBuf, true);
  if (changed) logEvent("Light schedule set: %sh on / %sh off", onBuf, offBuf);
}

// "HH:MM" (KSA): the lights switch on at this time every day
void setLightStart(const String& msg)
{
  int h = -1, m = -1;
  if (sscanf(msg.c_str(), "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return;
  if (!clockValid) { logEvent("Lights-on time not set - clock not synced yet"); return; }

  time_t now = nowEpoch();
  time_t ksaMidnight = now - ((now + KSA_OFFSET_SEC) % DAY_SEC);
  lightAnchor = (uint32_t)(ksaMidnight + h * 3600 + m * 60);
  preferences.putUInt("lightAnchor", lightAnchor);
  publishLightStart();
  logEvent("Lights-on time set to %s", fmt12(h, m));
}

void setPumpSchedule(int onMin, int offMin)
{
  onMin  = constrain(onMin,  MIN_PUMP_MIN, MAX_PUMP_MIN);
  offMin = constrain(offMin, MIN_PUMP_MIN, MAX_PUMP_MIN);
  bool changed = (onMin != pumpOnMin) || (offMin != pumpOffMin);
  pumpOnMin = onMin;
  pumpOffMin = offMin;
  preferences.putInt("pumpOnMin", pumpOnMin);
  preferences.putInt("pumpOffMin", pumpOffMin);
  // New timings start fresh from the beginning of an ON period
  if (clockValid) {
    pumpAnchor = (uint32_t)nowEpoch();
    preferences.putUInt("pumpAnchor", pumpAnchor);
  }
  publishPumpSchedule();
  if (changed) logEvent("Pump cycle set: %d min on / %d min off", pumpOnMin, pumpOffMin);
}

// Start from scratch: Day 1 begins now, lights and pump both start a fresh ON period.
void resetCycles()
{
  if (!clockValid) { logEvent("Reset not done - clock not synced yet"); return; }
  uint32_t now = (uint32_t)nowEpoch();
  lightAnchor = pumpAnchor = plantedAt = now;
  preferences.putUInt("lightAnchor", lightAnchor);
  preferences.putUInt("pumpAnchor", pumpAnchor);
  preferences.putUInt("plantedAt", plantedAt);
  publishLightStart();
  publishPlanted();
  int h, m;
  ksaClock(now, h, m);
  logEvent("Cycles restarted - lights now switch on daily at %s", fmt12(h, m));
}

// =====================================================================================
//  Clock sync: runs until the first successful NTP time
// =====================================================================================
void checkClock()
{
  if (clockValid) return;
  if (nowEpoch() < VALID_TIME) {
    // Not synced yet: relays hold their last saved states. Every 30 s (while WiFi is up)
    // restart NTP; from 60 s after boot also try the backup web-time source.
    static unsigned long lastKick = 0;
    if (WiFi.status() != WL_CONNECTED || millis() - lastKick < 30000) return;
    lastKick = millis();
    Serial.println("Clock not synced yet - retrying internet time");
    startNtp();
    if (millis() > 45000 && syncTimeFromHttp()) timeSource = "web server time";
    if (nowEpoch() < VALID_TIME) return;
  }
  clockValid = true;
  if (mqtt.connected()) mqtt.publish(CLOCK_STATE_TOPIC, "synced", true);
  uint32_t now = (uint32_t)nowEpoch();
  bool firstRun = false;
  if (!lightAnchor) { lightAnchor = now; preferences.putUInt("lightAnchor", lightAnchor); firstRun = true; }
  if (!pumpAnchor)  { pumpAnchor  = now; preferences.putUInt("pumpAnchor", pumpAnchor); }
  if (!plantedAt)   { plantedAt   = now; preferences.putUInt("plantedAt", plantedAt); }
  publishLightStart();
  publishPlanted();
  publishUptime();

  time_t k = (time_t)now + KSA_OFFSET_SEC;
  struct tm t;
  gmtime_r(&k, &t);
  static const char* MONTHS[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  logEvent("Clock synced: %d %s %d, %s KSA via %s%s", t.tm_mday, MONTHS[t.tm_mon], t.tm_year + 1900,
           fmt12(t.tm_hour, t.tm_min), timeSource, firstRun ? " (first run - schedule starts now)" : "");
}

// =====================================================================================
//  Schedules: computed from the real clock every loop, in BOTH modes.
//  In automatic mode they also drive the relays.
// =====================================================================================
void runSchedules()
{
  if (!clockValid) return;

  // ---- Lights: ON for onHours starting at the anchor time, repeating every 24h ----
  uint32_t onSec   = lightOnSec();
  uint32_t lightP  = cyclePos(lightAnchor, DAY_SEC);
  bool lightsPhaseOn = lightP < onSec;

  // ---- Pump: ON pumpOnMin, OFF pumpOffMin, repeating ----
  uint32_t pOnSec   = pumpOnSec();
  uint32_t pumpP    = cyclePos(pumpAnchor, pumpPeriod());
  bool pumpPhaseOn  = pumpP < pOnSec;

  if (autoMode) {
    bool mismatch = false;
    for (uint8_t i = 0; i < 4; i++) if (relayState[i] != (lightsPhaseOn && shelfEnabled[i])) mismatch = true;
    if (mismatch) setAllLeds(lightsPhaseOn, "schedule");

    if (pumpOverride == PUMP_AUTO) {
      bool pumpShould = pumpPhaseOn && tankFull;   // never run the pump dry automatically
      if (relayState[PUMP] != pumpShould) {
        setRelay(PUMP, pumpShould, true, false);   // routine cycling isn't logged
        if (!pumpShould && pumpPhaseOn && relayState[PUMP] == false && !tankFull) {
          static unsigned long lastEmptyLog = 0;
          if (millis() - lastEmptyLog > 60000) { lastEmptyLog = millis(); logEvent("Pump held off - tank empty"); }
        }
      }
    }
  }

  // ---- Broadcast both schedules once a second (for the Schedule tab) ----
  static unsigned long lastBroadcast = 0;
  if (millis() - lastBroadcast >= 1000) {
    lastBroadcast = millis();
    char payload[40];

    uint32_t lElapsed = lightsPhaseOn ? lightP : lightP - onSec;
    uint32_t lTotal   = lightsPhaseOn ? onSec : DAY_SEC - onSec;
    snprintf(payload, sizeof(payload), "%s,%lu,%lu", lightsPhaseOn ? "on" : "off",
             (unsigned long)lElapsed, (unsigned long)lTotal);
    mqtt.publish(CYCLE_STATUS_TOPIC, payload);

    const char* pPhase = pumpPhaseOn ? ((autoMode && !tankFull && pumpOverride == PUMP_AUTO) ? "blocked" : "on") : "off";
    uint32_t pElapsed = pumpPhaseOn ? pumpP : pumpP - pOnSec;
    uint32_t pTotal   = pumpPhaseOn ? pOnSec : (uint32_t)pumpOffMin * 60UL;
    snprintf(payload, sizeof(payload), "%s,%lu,%lu", pPhase, (unsigned long)pElapsed, (unsigned long)pTotal);
    mqtt.publish(PUMP_CYCLE_STATUS_TOPIC, payload);
  }
}

// Manual (demo) mode times out after 10 minutes
void checkManualTimeout()
{
  if (autoMode) return;
  unsigned long elapsed = millis() - manualStartMs;
  if (elapsed >= MANUAL_TIMEOUT_MS) {
    logEvent("Manual mode timed out");
    setMode(true);
    return;
  }
  static unsigned long lastBroadcast = 0;
  if (millis() - lastBroadcast >= 1000) {
    lastBroadcast = millis();
    mqtt.publish(MANUAL_REMAIN_TOPIC, String((MANUAL_TIMEOUT_MS - elapsed) / 1000).c_str());
  }
}

// =====================================================================================
//  Wireless firmware updates
// =====================================================================================
String pendingOtaUrl;          // set by the dashboard, installed from loop()
bool   lanOtaStarted = false;

void otaStatus(const char* msg)
{
  Serial.printf("[ota] %s\n", msg);
  if (mqtt.connected()) { mqtt.publish(OTA_STATE_TOPIC, msg); mqtt.loop(); }
}

// Remember the old version, so after the restart the log can say what changed
void noteUpdateStarting(const char* how)
{
  preferences.putString("otaFrom", FW_VERSION);
  preferences.putString("otaHow", how);
}

// Local-network updates from Arduino IDE (started once WiFi is up)
void startLanOta()
{
  if (lanOtaStarted) return;
  lanOtaStarted = true;
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { noteUpdateStarting("local network"); otaStatus("Local network update started"); });
  ArduinoOTA.onError([](ota_error_t e) {
    char b[48]; snprintf(b, sizeof(b), "Local network update failed (error %d)", (int)e);
    otaStatus(b); preferences.remove("otaFrom");
  });
  ArduinoOTA.begin();
  Serial.printf("[ota] Local network updates ready as \"%s\"\n", OTA_HOSTNAME);
}

// Updates from a URL (dashboard Admin tab). Blocks for the download (~10-30 s); the relays
// hold their states meanwhile, and the ESP32 restarts into the new firmware when done.
void runPendingOta()
{
  if (pendingOtaUrl.length() == 0) return;
  String url = pendingOtaUrl;
  pendingOtaUrl = "";

  if (!url.startsWith("https://") || !url.startsWith(OTA_URL_PREFIX) || !url.endsWith(".bin")) {
    otaStatus("Update refused: URL must be https, end in .bin and match OTA_URL_PREFIX");
    logEvent("Firmware update refused: URL not allowed");
    return;
  }
  logEvent("Firmware update started from dashboard");
  otaStatus("Downloading firmware...");
  noteUpdateStarting("dashboard");

  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(false);   // restart ourselves, after reporting success
  t_httpUpdate_return r = httpUpdate.update(client, url);

  if (r == HTTP_UPDATE_OK) {
    otaStatus("Update installed - restarting");
    delay(500);
    ESP.restart();
  }
  preferences.remove("otaFrom");
  String err = (r == HTTP_UPDATE_NO_UPDATES) ? String("no update") : httpUpdate.getLastErrorString();
  char b[110];
  snprintf(b, sizeof(b), "Firmware update failed: %s", err.c_str());
  otaStatus(b);
  logEvent("%s", b);
}

// After a restart: if an update was just installed, say so in the log
void reportFinishedUpdate()
{
  String from = preferences.getString("otaFrom", "");
  if (from.length() == 0) return;
  String how = preferences.getString("otaHow", "wireless");
  preferences.remove("otaFrom");
  if (from == FW_VERSION) logEvent("Firmware reinstalled (%s), still version %s", how.c_str(), FW_VERSION);
  else logEvent("Firmware updated %s -> %s (%s)", from.c_str(), FW_VERSION, how.c_str());
}

// =====================================================================================
//  MQTT
// =====================================================================================
void onMqttMessage(char* topic, byte* payload, unsigned int length)
{
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  if (strcmp(topic, MODE_SET_TOPIC) == 0)          { setMode(msg == "auto"); return; }
  if (strcmp(topic, SCHEDULE_SET_TOPIC) == 0)      { setSchedule(msg.toFloat()); return; }
  if (strcmp(topic, LIGHT_START_SET_TOPIC) == 0)   { setLightStart(msg); return; }
  if (strcmp(topic, CYCLE_RESET_TOPIC) == 0)       { resetCycles(); return; }
  if (strcmp(topic, PUMP_SCHED_SET_TOPIC) == 0) {
    int onMin = 0, offMin = 0;
    if (sscanf(msg.c_str(), "%d,%d", &onMin, &offMin) == 2) setPumpSchedule(onMin, offMin);
    return;
  }
  for (uint8_t i = 0; i < 4; i++) {
    if (strcmp(topic, ENABLED_SET_TOPICS[i]) == 0) { setShelfEnabled(i, msg == "1"); return; }
  }
  if (strcmp(topic, OTA_SET_TOPIC) == 0) { pendingOtaUrl = msg; return; }
  if (strcmp(topic, PUMP_OVERRIDE_SET_TOPIC) == 0) {
    setPumpOverride(msg == "on" ? PUMP_FORCE_ON : msg == "off" ? PUMP_FORCE_OFF : PUMP_AUTO);
    return;
  }
  for (uint8_t i = 0; i < 5; i++) {
    if (strcmp(topic, SET_TOPICS[i]) == 0) {
      // In automatic mode the schedules own the relays: ignore, and re-publish the true
      // state so the dashboard button snaps back.
      if (autoMode) {
        mqtt.publish(STATE_TOPICS[i], relayState[i] ? "1" : "0", true);
        return;
      }
      if (i == PUMP) {
        if (pumpOverride != PUMP_AUTO) {                  // maintenance switch owns the pump
          mqtt.publish(STATE_TOPICS[i], relayState[i] ? "1" : "0", true);
          return;
        }
        if (msg == "1" && !tankFull) {                    // never start the pump on an empty tank
          mqtt.publish(STATE_TOPICS[i], "0", true);
          logEvent("Pump not started - tank empty");
          return;
        }
      }
      setRelay(i, msg == "1");
      return;
    }
  }
}

// "18h 29m", "3m 05s", "2d 4h"
void fmtDuration(unsigned long s, char* out, size_t n)
{
  if (s >= 86400UL)     snprintf(out, n, "%lud %luh", s / 86400UL, (s % 86400UL) / 3600UL);
  else if (s >= 3600UL) snprintf(out, n, "%luh %lum", s / 3600UL, (s % 3600UL) / 60UL);
  else                  snprintf(out, n, "%lum %02lus", s / 60UL, s % 60UL);
}

void onMqttConnected()
{
  Serial.println("MQTT connected");
  mqttConnectedAtMs = millis();
  mqtt.publish(STATUS_TOPIC, "online", true);
  mqtt.publish(CLOCK_STATE_TOPIC, clockValid ? "synced" : "waiting", true);
  if (clockValid) {
    publishUptime();
  } else {
    // Clear uptime from a previous run so the dashboard doesn't show stale numbers
    mqtt.publish(BOOT_TIME_TOPIC, "", true);
    mqtt.publish(CONNECTED_TIME_TOPIC, "", true);
  }
  publishAllStates();
  for (uint8_t i = 0; i < 5; i++) mqtt.subscribe(SET_TOPICS[i]);
  mqtt.subscribe(MODE_SET_TOPIC);
  mqtt.subscribe(SCHEDULE_SET_TOPIC);
  mqtt.subscribe(LIGHT_START_SET_TOPIC);
  mqtt.subscribe(PUMP_SCHED_SET_TOPIC);
  mqtt.subscribe(CYCLE_RESET_TOPIC);
  for (uint8_t i = 0; i < 4; i++) mqtt.subscribe(ENABLED_SET_TOPICS[i]);
  mqtt.subscribe(PUMP_OVERRIDE_SET_TOPIC);
  mqtt.subscribe(OTA_SET_TOPIC);
  mqtt.publish(FIRMWARE_STATE_TOPIC, FW_VERSION, true);

  if (mqttLostAt != 0) {
    char dur[24];
    fmtDuration((millis() - mqttLostAt) / 1000, dur, sizeof(dur));
    char why[40];
    if (offlineSawWifiDrop) snprintf(why, sizeof(why), "WiFi dropped");
    else                    snprintf(why, sizeof(why), "WiFi up, broker unreachable rc=%d", lastMqttRc);
    logEvent("Back online after %s: %s, %d WiFi restarts, signal %ld dBm",
             dur, why, wifiResets, (long)WiFi.RSSI());
  }
}

// Called once when the connection is lost. The log line is queued and uploaded once the
// connection is back, so it carries the real time of the drop in its text.
void markDisconnected(bool wifiDropped)
{
  if (wifiDropped) offlineSawWifiDrop = true;
  if (!mqttWasConnected) return;
  mqttWasConnected = false;
  mqttLostAt = millis();
  offlineSawWifiDrop = wifiDropped;
  wifiResets = 0;
  lastMqttRc = mqtt.state();
  Serial.println(wifiDropped ? "WiFi lost" : "MQTT connection lost");
  if (clockValid) {
    int h, m;
    ksaClock((uint32_t)nowEpoch(), h, m);
    logEvent("Went offline at %s KSA: %s, last signal %ld dBm",
             fmt12(h, m), wifiDropped ? "WiFi dropped" : "broker connection lost", lastRssi);
  }
}

void restartWifi(const char* why)
{
  Serial.printf("WiFi: full restart (%s)\n", why);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifiResets++;
}

// Keeps WiFi and MQTT connected without ever blocking the schedules:
//  - WiFi down: WiFi.reconnect() every 15 s; every 2 min a full WiFi restart
//  - WiFi "up" but the broker unreachable for 5 min: full WiFi restart, which also gets a
//    fresh IP address (fixes a stale link where WiFi looks connected but nothing gets through)
void maintainConnections()
{
  static unsigned long lastWifiKick = 0, lastWifiFullReset = 0;
  static unsigned long lastMqttAttempt = 0, mqttFailingSince = 0, lastRssiRead = 0;
  static bool triedMqttOnce = false, wifiWasUp = false;

  bool wifiUp = WiFi.status() == WL_CONNECTED;
  if (wifiUp && !wifiWasUp && !clockValid) startNtp();      // ask for the time once WiFi is really up
  if (wifiUp) startLanOta();                                 // local-network updates (once)
  if (!wifiUp && wifiWasUp) lastWifiFullReset = millis();   // start the 2-minute timer at the drop
  wifiWasUp = wifiUp;

  if (!wifiUp) {
    markDisconnected(true);
    mqttFailingSince = 0;
    if (millis() - lastWifiFullReset >= WIFI_FULL_RESET_MS) {
      lastWifiFullReset = lastWifiKick = millis();
      restartWifi("down for 2 min");
    } else if (millis() - lastWifiKick >= WIFI_RETRY_MS) {
      lastWifiKick = millis();
      Serial.println("WiFi: trying to reconnect...");
      WiFi.reconnect();
    }
    return;
  }

  if (millis() - lastRssiRead > 30000) { lastRssiRead = millis(); lastRssi = WiFi.RSSI(); }

  if (mqtt.connected()) { mqttFailingSince = 0; return; }
  markDisconnected(false);

  if (!mqttFailingSince) mqttFailingSince = millis();
  if (millis() - mqttFailingSince >= MQTT_STALE_RESET_MS) {
    mqttFailingSince = millis();
    restartWifi("broker unreachable for 5 min");
    return;
  }

  if (triedMqttOnce && millis() - lastMqttAttempt < MQTT_RETRY_MS) return;
  triedMqttOnce = true;
  lastMqttAttempt = millis();

  Serial.print("Connecting to MQTT...");
  if (mqtt.connect(CLIENT_ID, MQTT_USER, MQTT_PASS, STATUS_TOPIC, 1, true, "offline")) {
    mqttWasConnected = true;
    mqttFailingSince = 0;
    onMqttConnected();
  } else {
    lastMqttRc = mqtt.state();
    Serial.print("failed, rc=");
    Serial.println(lastMqttRc);
  }
}

// =====================================================================================
void setup()
{
  Serial.begin(115200);
  preferences.begin("hydroponics", false);

  for (uint8_t i = 0; i < 5; i++) {
    digitalWrite(RELAY_PINS[i], relayLevel(i, false));
    pinMode(RELAY_PINS[i], OUTPUT);
  }

  pinMode(FLOAT_PIN, INPUT_PULLUP);
  delay(50);
  initTankSensor();

  // Until the clock is synced, the relays hold the last saved states
  loadStates();
  applyLoadedStates();
  manualStartMs = millis();

  if (cloudLoggingEnabled()) {
    logQueue = xQueueCreate(LOG_QUEUE_LEN, LOG_MSG_LEN);
    xTaskCreatePinnedToCore(logTask, "logTask", 12288, nullptr, 1, nullptr, 0);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // Internet time (UTC). Requested again when WiFi connects and every 30 s until it works;
  // once synced it re-syncs automatically about once an hour.
  startNtp();

  wifiClient.setInsecure();
  wifiClient.setHandshakeTimeout(10);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);

  logEvent("Device started (%s), firmware %s, tank %s, %s mode",
           resetReasonText(), FW_VERSION, tankFull ? "full" : "empty", autoMode ? "auto" : "manual");
  reportFinishedUpdate();
}

void loop()
{
  maintainConnections();
  mqtt.loop();
  if (lanOtaStarted) ArduinoOTA.handle();
  runPendingOta();
  checkClock();
  updateTankSensor();
  runSchedules();
  enforcePump();
  checkManualTimeout();
}
