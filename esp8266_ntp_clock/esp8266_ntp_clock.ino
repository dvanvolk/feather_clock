// ============================================================
//  ESP8266 + DS3231 RTC + Adafruit Quad Alphanumeric Clock
// ============================================================
//
//  Strategy:
//    - DS3231 always stores UTC (no DST confusion in hardware)
//    - DST/timezone applied at display time via POSIX tz rules
//    - NTP syncs UTC into DS3231 once per day:
//        * corrects DS3231 drift
//        * picks up DST transitions automatically
//    - On boot, if DS3231 is valid, display starts immediately
//      with no WiFi needed.
//    - Every 5 minutes: connect WiFi, publish RTC temperature,
//      last NTP sync time, and RSSI to an MQTT broker, then
//      disconnect.
// ============================================================

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include "Adafruit_LEDBackpack.h"
#include "RTClib.h"             // Adafruit RTClib — covers DS3231
#define MQTT_MAX_PACKET_SIZE 512  // must precede PubSubClient include for older library versions
#include <PubSubClient.h>
#include "secrets.h"            // WiFi/MQTT credentials (not checked into source control)

// ------------------------------------------------------------
// User configuration
// ------------------------------------------------------------
const char* WIFI_SSID      = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD  = SECRET_WIFI_PASSWORD;
const char* DEVICE_NAME    = SECRET_DEVICE_NAME;

// NTP servers tried in order — IP addresses avoid DNS dependency
const char* NTP_SERVERS[]  = {
  "216.239.35.0",   // time1.google.com
  "216.239.35.4",   // time2.google.com
  "129.6.15.28",    // time.nist.gov
  "pool.ntp.org"
};
const int NTP_SERVER_COUNT = 4;

// POSIX timezone string — handles DST transitions automatically.
// Eastern:  "EST5EDT,M3.2.0,M11.1.0"
// Central:  "CST6CDT,M3.2.0,M11.1.0"
// Mountain: "MST7MDT,M3.2.0,M11.1.0"
// Pacific:  "PST8PDT,M3.2.0,M11.1.0"
const char* POSIX_TZ = "EST5EDT,M3.2.0,M11.1.0";

// MQTT broker — change MQTT_PORT here if your broker uses a non-standard port
const char* MQTT_HOST     = SECRET_MQTT_HOST;
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = SECRET_MQTT_USER;
const char* MQTT_PASSWORD = SECRET_MQTT_PASSWORD;

// State topics:     feather_clock/<device_name>/<sensor>
// Discovery topics: homeassistant/sensor/<device_name>_<sensor>/config
const char* MQTT_PREFIX = "feather_clock";

// ------------------------------------------------------------
// Timing constants
// ------------------------------------------------------------
const unsigned long CLOCK_UPDATE_MS  = 1000UL;
const unsigned long NTP_RESYNC_MS    = 24UL * 60 * 60 * 1000;
const unsigned long MQTT_REPORT_MS   = 5UL  * 60 * 1000;
const unsigned int  WIFI_TIMEOUT_MS  = 10000;
const unsigned int  WIFI_RETRY_MS    = 500;
const unsigned long NTP_WAIT_MS      = 15000;

// ------------------------------------------------------------
// Display constants
// ------------------------------------------------------------
const uint8_t DISPLAY_I2C_ADDR = 0x70;
const uint8_t DIGIT_HOUR_TENS  = 0;
const uint8_t DIGIT_HOUR_ONES  = 1;
const uint8_t DIGIT_MIN_TENS   = 2;
const uint8_t DIGIT_MIN_ONES   = 3;
const bool    SHOW_DOT         = true;  // Dot on digit 1 acts as the colon separator

// Epoch time at Jan 1 2020 — any valid NTP response will be larger than this
const unsigned long EPOCH_2020 = 1577836800UL;

#define C_TO_F(c) ((c) * 9.0f / 5.0f + 32.0f)

// ------------------------------------------------------------
// Globals
// ------------------------------------------------------------
Adafruit_AlphaNum4 display = Adafruit_AlphaNum4();
RTC_DS3231         rtc;
WiFiClient         wifiClient;
PubSubClient       mqtt(wifiClient);
unsigned long      lastClockUpdate  = 0;
unsigned long      lastNtpSync      = 0;
unsigned long      lastMqttReport   = 0;
int                lastDisplayedMin = -1;
char               lastNtpSyncStr[32] = "Never";  // ISO 8601 local time of last NTP sync

// ------------------------------------------------------------
// Display helpers
// ------------------------------------------------------------

void showMessage(const char* msg) {
  for (uint8_t i = 0; i < 4; i++) {
    display.writeDigitAscii(i, msg[i] ? msg[i] : ' ');
  }
  display.writeDisplay();
}

void showTime(int hour24, int minute) {
  int hour12 = hour24 % 12;
  if (hour12 == 0) hour12 = 12;

  char tensHour = (hour12 >= 10) ? ('0' + hour12 / 10) : ' ';
  char onesHour =  '0' + hour12 % 10;
  char tensMins =  '0' + minute / 10;
  char onesMins =  '0' + minute % 10;

  display.writeDigitAscii(DIGIT_HOUR_TENS, tensHour);
  display.writeDigitAscii(DIGIT_HOUR_ONES, onesHour, SHOW_DOT);
  display.writeDigitAscii(DIGIT_MIN_TENS,  tensMins);
  display.writeDigitAscii(DIGIT_MIN_ONES,  onesMins);
  display.writeDisplay();
}

// ------------------------------------------------------------
// Timezone helper
// ------------------------------------------------------------

// Convert a UTC Unix timestamp to local time, applying DST via
// the POSIX_TZ rule string. Returns a broken-down struct tm.
struct tm utcToLocal(time_t utc) {
  setenv("TZ", POSIX_TZ, 1);
  tzset();
  struct tm local;
  localtime_r(&utc, &local);
  return local;
}

// ------------------------------------------------------------
// WiFi helpers
// ------------------------------------------------------------

void connectWiFi() {
  Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.println("WiFi connection timed out.");
      return;
    }
    delay(WIFI_RETRY_MS);
    Serial.print(".");
  }
  Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
}

void disconnectWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WiFi disconnected.");
}

// ------------------------------------------------------------
// NTP helpers
// ------------------------------------------------------------

// Fetch UTC time from NTP and return as Unix epoch, or 0 on failure.
// configTime(0, 0, ...) retrieves raw UTC — timezone applied separately.
time_t fetchNTP() {
  delay(500);  // Let the stack settle after WiFi connect

  for (int i = 0; i < NTP_SERVER_COUNT; i++) {
    Serial.printf("Trying NTP server: %s\n", NTP_SERVERS[i]);
    configTime(0, 0, NTP_SERVERS[i]);  // UTC only — no offset here

    unsigned long start = millis();
    time_t utc = time(nullptr);
    while (utc < EPOCH_2020 && millis() - start < NTP_WAIT_MS) {
      delay(100);
      Serial.print(".");
      utc = time(nullptr);
    }
    Serial.println();

    if (utc >= EPOCH_2020) {
      Serial.printf("NTP UTC: %s", ctime(&utc));
      return utc;
    }
    Serial.printf("No response from %s, trying next...\n", NTP_SERVERS[i]);
  }

  Serial.println("NTP sync failed: all servers timed out.");
  return 0;
}

// Sync UTC from NTP into the DS3231, then log the local time.
bool syncNTP() {
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("NTP sync skipped: no WiFi.");
    return false;
  }

  time_t utc = fetchNTP();

  if (utc == 0) {
    disconnectWiFi();
    return false;
  }

  // Store raw UTC in the DS3231 — timezone is applied at display time
  rtc.adjust(DateTime(utc));

  struct tm local = utcToLocal(utc);
  // ISO 8601 format — required by HA's timestamp device class
  snprintf(lastNtpSyncStr, sizeof(lastNtpSyncStr), "%04d-%02d-%02dT%02d:%02d:%02d",
           local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
           local.tm_hour, local.tm_min, local.tm_sec);

  Serial.printf("DS3231 updated. Local time: %s (DST %s)\n",
                lastNtpSyncStr, local.tm_isdst ? "ON" : "OFF");

  // Report to MQTT immediately after NTP sync while WiFi is already up
  reportToMQTT();

  disconnectWiFi();
  return true;
}

// ------------------------------------------------------------
// RTC helpers
// ------------------------------------------------------------

// Returns true if the DS3231 holds a plausible UTC time (after 2020).
bool rtcIsValid() {
  if (rtc.lostPower()) {
    Serial.println("DS3231 lost power — time is not set.");
    return false;
  }
  DateTime now = rtc.now();
  if (now.year() < 2020) {
    Serial.println("DS3231 time predates 2020 — treating as unset.");
    return false;
  }
  return true;
}

// ------------------------------------------------------------
// MQTT helpers
// ------------------------------------------------------------

// Connect to the MQTT broker. Returns true on success.
bool connectMQTT() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);  // for discovery payloads; belt-and-suspenders with the #define above

  String clientId = String("feather_clock_") + DEVICE_NAME;
  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
    Serial.println("MQTT connected.");
    return true;
  }
  Serial.printf("MQTT connect failed, rc=%d\n", mqtt.state());
  return false;
}

// Publish HA MQTT discovery payloads so Home Assistant auto-discovers all
// three sensors without manual YAML configuration. Published with retain=true
// so HA picks them up after a restart even before the next report cycle.
void publishDiscovery() {
  String base        = String(DEVICE_NAME);
  String deviceLabel = base;
  deviceLabel.replace("_", " ");
  deviceLabel[0]     = toupper(deviceLabel[0]);

  String stateBase = String(MQTT_PREFIX) + "/" + base + "/";
  String discBase  = "homeassistant/sensor/" + base + "_";

  struct Sensor { const char* id; const char* name; const char* unit; const char* cls; };
  Sensor sensors[] = {
    { "rtc_temperature", "RTC Temperature", "\xc2\xb0""F", "temperature"     },
    { "wifi_rssi",       "WiFi RSSI",       "dBm",          "signal_strength" },
    { "last_ntp_sync",   "Last NTP Sync",   "",             "timestamp"       },
  };

  for (auto& s : sensors) {
    String payload = "{\"name\":\"" + deviceLabel + " " + s.name + "\","
                     "\"state_topic\":\"" + stateBase + s.id + "\","
                     "\"unique_id\":\"" + base + "_" + s.id + "\"";
    if (strlen(s.unit) > 0)
      payload += ",\"unit_of_measurement\":\"" + String(s.unit) + "\"";
    if (strlen(s.cls) > 0)
      payload += ",\"device_class\":\"" + String(s.cls) + "\"";
    payload += ",\"device\":{\"identifiers\":[\"" + base + "\"],"
               "\"name\":\"" + deviceLabel + "\",\"model\":\"Feather Clock\"}}";

    mqtt.publish((discBase + s.id + "/config").c_str(), payload.c_str(), true);
    Serial.printf("Discovery: %s\n", s.id);
  }
}

// Publish sensor states to MQTT, assuming WiFi is already connected.
// Handles MQTT connect/disconnect internally.
void reportToMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("MQTT report skipped: no WiFi.");
    return;
  }

  if (!connectMQTT()) return;

  publishDiscovery();

  String prefix = String(MQTT_PREFIX) + "/" + DEVICE_NAME + "/";

  float tempF = C_TO_F(rtc.getTemperature());
  char  tempStr[8];
  snprintf(tempStr, sizeof(tempStr), "%.2f", tempF);
  mqtt.publish((prefix + "rtc_temperature").c_str(), tempStr, true);

  mqtt.publish((prefix + "last_ntp_sync").c_str(), lastNtpSyncStr, true);

  char rssiStr[8];
  snprintf(rssiStr, sizeof(rssiStr), "%d", WiFi.RSSI());
  mqtt.publish((prefix + "wifi_rssi").c_str(), rssiStr, true);

  mqtt.disconnect();
  Serial.println("MQTT report complete.");
}

// Connect WiFi, report to MQTT, disconnect.
void reportToMQTTWithWiFi() {
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) return;
  reportToMQTT();
  disconnectWiFi();
}

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Feather Clock starting ===");

  display.begin(DISPLAY_I2C_ADDR);
  showMessage("----");

  if (!rtc.begin()) {
    Serial.println("DS3231 not found — check wiring!");
    showMessage("rtc ");
    while (true) delay(1000);  // Halt — nothing works without the RTC
  }

  // Log DS3231 temperature as a hardware sanity check
  Serial.printf("DS3231 temperature: %.2f °C\n", rtc.getTemperature());

  if (!rtcIsValid()) {
    Serial.println("RTC unset — syncing from NTP.");
    if (!syncNTP()) {
      showMessage("Err ");
      // Continue — RTC will be corrected on next daily sync
    }
  } else {
    // RTC is valid — log current local time and start immediately
    time_t utc = rtc.now().unixtime();
    struct tm local = utcToLocal(utc);
    Serial.printf("RTC valid. Local time: %02d:%02d:%02d (DST %s)\n",
                  local.tm_hour, local.tm_min, local.tm_sec,
                  local.tm_isdst ? "ON" : "OFF");
  }

  lastNtpSync     = millis();
  lastMqttReport  = millis();
  lastClockUpdate = millis();
}

// ------------------------------------------------------------
// Main loop
// ------------------------------------------------------------

void loop() {
  unsigned long now = millis();

  // Sync DS3231 from NTP once per day — corrects drift and picks up DST changes
  if (now - lastNtpSync >= NTP_RESYNC_MS) {
    syncNTP();  // Also reports to MQTT while WiFi is up
    lastNtpSync    = millis();
    lastMqttReport = millis();  // Reset MQTT timer so we don't double-report
  }

  // Publish sensor data to MQTT every 5 minutes
  if (now - lastMqttReport >= MQTT_REPORT_MS) {
    reportToMQTTWithWiFi();
    lastMqttReport = millis();
  }

  // Update display once per second — read UTC from DS3231, convert to local
  if (now - lastClockUpdate >= CLOCK_UPDATE_MS) {
    lastClockUpdate = millis();
    time_t utc = rtc.now().unixtime();
    struct tm local = utcToLocal(utc);
    showTime(local.tm_hour, local.tm_min);
    if (local.tm_min != lastDisplayedMin) {
      lastDisplayedMin = local.tm_min;
      Serial.printf("Display: %02d:%02d (DST %s)\n",
                    local.tm_hour, local.tm_min, local.tm_isdst ? "ON" : "OFF");
    }
  }
}
