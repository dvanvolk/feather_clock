# Feather Clock

An ESP8266-based NTP clock that displays the current local time on an Adafruit Quad Alphanumeric FeatherWing and reports sensor data to Home Assistant.

## Hardware

| Component | Part |
|-----------|------|
| Microcontroller | Adafruit Feather HUZZAH (ESP8266) |
| Real-time clock | DS3231 RTC module (I2C) |
| Display | Adafruit 0.54" Quad Alphanumeric FeatherWing (14-segment, HT16K33) |

## Features

- Displays 12-hour local time with automatic DST handling via POSIX timezone rules
- DS3231 RTC keeps time when WiFi is unavailable — clock works immediately on boot if the RTC holds a valid time
- NTP sync once per day corrects RTC drift and picks up DST transitions automatically
- Reports to Home Assistant every 5 minutes:
  - DS3231 on-chip temperature
  - WiFi signal strength (RSSI)
  - Timestamp of last NTP sync
- WiFi connects only when needed for NTP or Home Assistant, then disconnects

## Setup

### 1. Install dependencies

Install the following libraries via the Arduino Library Manager:

- `Adafruit GFX Library`
- `Adafruit LED Backpack Library`
- `RTClib` (by Adafruit)
- ESP8266 board package (`esp8266:esp8266`) — [install guide](https://arduino-esp8266.readthedocs.io/en/latest/installing.html)

### 2. Configure secrets

Copy `secrets.h.template` to `secrets.h` and fill in your values:

```bash
cp secrets.h.template secrets.h
```

Edit `secrets.h`:

```cpp
#define SECRET_DEVICE_NAME   "kitchen_clock"   // lowercase + underscores only
#define SECRET_WIFI_SSID     "your_ssid"
#define SECRET_WIFI_PASSWORD "your_password"
#define SECRET_HA_TOKEN      "your_ha_token"   // Home Assistant long-lived access token
```

`DEVICE_NAME` drives the Home Assistant entity IDs — e.g. `kitchen_clock` creates `sensor.kitchen_clock_rtc_temperature`.

### 3. Set your timezone

In `esp8266_ntp_clock.ino`, update `POSIX_TZ` to match your timezone:

```cpp
// Eastern:  "EST5EDT,M3.2.0,M11.1.0"
// Central:  "CST6CDT,M3.2.0,M11.1.0"
// Mountain: "MST7MDT,M3.2.0,M11.1.0"
// Pacific:  "PST8PDT,M3.2.0,M11.1.0"
const char* POSIX_TZ = "EST5EDT,M3.2.0,M11.1.0";
```

### 4. Build and flash

```bash
# Compile
arduino-cli compile --fqbn esp8266:esp8266:huzzah feather_clock

# Upload (replace port as needed)
arduino-cli upload --fqbn esp8266:esp8266:huzzah --port /dev/cu.usbserial-XXXX feather_clock

# Monitor serial output
arduino-cli monitor --port /dev/cu.usbserial-XXXX --config baudrate=115200
```

## Home Assistant

The clock posts three sensor entities to the [Home Assistant REST API](https://developers.home-assistant.io/docs/api/rest/). Generate a long-lived access token under your HA profile and set `HA_HOST` in the sketch if your instance isn't at `homeassistant.local:8123`.

| Entity | Unit | Description |
|--------|------|-------------|
| `sensor.<name>_rtc_temperature` | °C | DS3231 on-chip temperature |
| `sensor.<name>_wifi_rssi` | dBm | WiFi signal strength |
| `sensor.<name>_last_ntp_sync` | — | Local timestamp of last NTP sync |
