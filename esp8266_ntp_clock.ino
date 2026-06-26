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
//    - Every 5 minutes: connect WiFi, post RTC temperature,
//      last NTP sync time, and RSSI to Home Assistant, then
//      disconnect.
// ============================================================

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include "Adafruit_LEDBackpack.h"
#include "RTClib.h"             // Adafruit RTClib — covers DS3231
#include "secrets.h"            // WiFi credentials and API tokens (not checked into source control)

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

// Home Assistant REST API
// Token: Profile → Long-Lived Access Tokens → Create Token
const char* HA_HOST  = "http://homeassistant.local:8123";
const char* HA_TOKEN = SECRET_HA_TOKEN;

// ------------------------------------------------------------
// Timing constants
// ------------------------------------------------------------
const unsigned long CLOCK_UPDATE_MS  = 1000UL;
const unsigned long NTP_RESYNC_MS    = 24UL * 60 * 60 * 1000;
const unsigned long HA_REPORT_MS     = 5UL  * 60 * 1000;
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

// ------------------------------------------------------------
// Globals
// ------------------------------------------------------------
Adafruit_AlphaNum4 display = Adafruit_AlphaNum4();
RTC_DS3231         rtc;
unsigned long      lastClockUpdate = 0;
unsigned long      lastNtpSync     = 0;
unsigned long      lastHaReport    = 0;
char               lastNtpSyncStr[32] = "Never";  // Human-readable last sync time

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
  snprintf(lastNtpSyncStr, sizeof(lastNtpSyncStr), "%04d-%02d-%02d %02d:%02d:%02d",
           local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
           local.tm_hour, local.tm_min, local.tm_sec);

  Serial.printf("DS3231 updated. Local time: %s (DST %s)\n",
                lastNtpSyncStr, local.tm_isdst ? "ON" : "OFF");

  // Report to HA immediately after NTP sync while WiFi is already up
  reportToHomeAssistant();

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
// Home Assistant helpers
// ------------------------------------------------------------

// POST a single sensor state to the Home Assistant REST API.
bool postToHA(WiFiClient& wifiClient, const char* entityId,
              const char* state, const char* unit,
              const char* friendlyName, const char* deviceClass) {
  HTTPClient http;
  String url = String(HA_HOST) + "/api/states/" + entityId;

  http.begin(wifiClient, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);

  // Build JSON payload with state and attributes
  String payload = "{\"state\":\"" + String(state) + "\","
                   "\"attributes\":{"
                   "\"unit_of_measurement\":\"" + unit + "\","
                   "\"friendly_name\":\"" + friendlyName + "\","
                   "\"device_class\":\"" + deviceClass + "\"}}";

  int httpCode = http.POST(payload);
  bool success = (httpCode == 200 || httpCode == 201);

  Serial.printf("HA POST %s → HTTP %d\n", entityId, httpCode);
  http.end();
  return success;
}

void reportToHomeAssistant() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("HA report skipped: no WiFi.");
    return;
  }

  WiFiClient wifiClient;

  // Build entity IDs and friendly names from the device name so
  // multiple clocks post to separate sensors in Home Assistant.
  String prefix      = String("sensor.") + DEVICE_NAME + "_";
  String namePrefix  = String(DEVICE_NAME);
  namePrefix.replace("_", " ");  // "kitchen_clock" → "Kitchen clock"
  namePrefix[0] = toupper(namePrefix[0]);

  // DS3231 temperature
  float tempC = rtc.getTemperature();
  char tempStr[8];
  snprintf(tempStr, sizeof(tempStr), "%.2f", tempC);
  postToHA(wifiClient, (prefix + "rtc_temperature").c_str(),
           tempStr, "°C", (namePrefix + " RTC Temperature").c_str(), "temperature");

  // Last NTP sync timestamp
  postToHA(wifiClient, (prefix + "last_ntp_sync").c_str(),
           lastNtpSyncStr, "", (namePrefix + " Last NTP Sync").c_str(), "timestamp");

  // WiFi signal strength
  char rssiStr[8];
  snprintf(rssiStr, sizeof(rssiStr), "%d", WiFi.RSSI());
  postToHA(wifiClient, (prefix + "wifi_rssi").c_str(),
           rssiStr, "dBm", (namePrefix + " WiFi RSSI").c_str(), "signal_strength");

  Serial.println("HA report complete.");
}

// Connect WiFi, report to HA, disconnect.
void reportToHomeAssistantWithWiFi() {
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) return;
  reportToHomeAssistant();
  disconnectWiFi();
}

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(500);

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
  lastHaReport    = millis();
  lastClockUpdate = millis();
}

// ------------------------------------------------------------
// Main loop
// ------------------------------------------------------------

void loop() {
  unsigned long now = millis();

  // Sync DS3231 from NTP once per day — corrects drift and picks up DST changes
  if (now - lastNtpSync >= NTP_RESYNC_MS) {
    syncNTP();  // Also reports to HA while WiFi is up
    lastNtpSync  = millis();
    lastHaReport = millis();  // Reset HA timer so we don't double-report
  }

  // Report to Home Assistant every 5 minutes
  if (now - lastHaReport >= HA_REPORT_MS) {
    reportToHomeAssistantWithWiFi();
    lastHaReport = millis();
  }

  // Update display once per second — read UTC from DS3231, convert to local
  if (now - lastClockUpdate >= CLOCK_UPDATE_MS) {
    lastClockUpdate = millis();
    time_t utc = rtc.now().unixtime();
    struct tm local = utcToLocal(utc);
    showTime(local.tm_hour, local.tm_min);
  }
}
