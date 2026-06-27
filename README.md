# Feather Clock

An ESP8266-based NTP clock that displays the current local time on an Adafruit Quad Alphanumeric FeatherWing and reports temperature, humidity, and status to Home Assistant via MQTT.

## Hardware

| Component | Part |
|-----------|------|
| Microcontroller | Adafruit Feather HUZZAH (ESP8266) |
| Real-time clock | DS3231 RTC module (I2C) |
| Display | Adafruit 0.54" Quad Alphanumeric FeatherWing (14-segment, HT16K33) |
| Temp / Humidity | DHT22 sensor (GPIO14) |

## Wiring

### Overview

```
               Feather HUZZAH ESP8266
              ┌──────────────────────┐
              │                      │
    ┌─────────┤ 3V3            GPIO14├─────────────────────┐
    │  ┌──────┤ GND              GND ├──────────────────┐  │
    │  │      │ SDA (GPIO4)          │                  │  │
    │  │      │ SCL (GPIO5)          │                  │  │
    │  │      └──────────────────────┘                  │  │
    │  │                                                 │  │
    │  │      I2C bus (SDA + SCL shared)                 │  │
    │  │      ┌──────────┐   ┌────────────────────────┐ │  │
    │  └──────┤ GND      │   │  AlphaNum4 FeatherWing  │ │  │
    └─────────┤ VCC      │   │  (stacks on headers —  │ │  │
              │ SDA ─────┤   │   no separate wiring)  │ │  │
              │ SCL      │   │   I2C address 0x70     │ │  │
              └──────────┘   └────────────────────────┘ │  │
               DS3231 RTC                                │  │
                                                         │  │
              DHT22                                      │  │
              ┌───────────┐                              │  │
              │ pin 1 VCC ├──────────────────────────────┘  │
              │ pin 2 DAT ├──────────────────────────────────┘
              │ pin 3 NC  │    also connect a 10 kΩ resistor
              │ pin 4 GND ├──── between pin 2 (DAT) and 3V3
              └───────────┘
```

### DS3231 RTC — I2C

| DS3231 pin | Feather HUZZAH pin |
|------------|--------------------|
| VCC        | 3V3                |
| GND        | GND                |
| SDA        | SDA (GPIO4)        |
| SCL        | SCL (GPIO5)        |

### AlphaNum4 FeatherWing — I2C (0x70)

The FeatherWing stacks directly on the Feather via the stacking headers — no separate wires are needed. It shares the same I2C bus as the DS3231.

### DHT22 — GPIO14

| DHT22 pin   | Connection |
|-------------|------------|
| 1 (VCC)     | 3V3        |
| 2 (Data)    | GPIO14 + 10 kΩ pull-up resistor to 3V3 |
| 3 (NC)      | — not connected |
| 4 (GND)     | GND        |

> The 10 kΩ pull-up between the Data pin and 3V3 is required for reliable DHT22 readings.

## Features

- Displays 12-hour local time with automatic DST handling via POSIX timezone rules
- DS3231 RTC keeps time when WiFi is unavailable — clock works immediately on boot if the RTC holds a valid time
- NTP sync once per day corrects RTC drift and picks up DST transitions automatically
- Reports to Home Assistant every 5 minutes via MQTT:
  - Ambient temperature and humidity (DHT22)
  - DS3231 on-chip temperature (note: runs ~5–10 °F high due to self-heating)
  - WiFi signal strength (RSSI)
  - Timestamp of last NTP sync
- WiFi connects only when needed for NTP or MQTT reporting, then disconnects
- Blocking WiFi operations are gated to seconds :10–:45 of each minute so the display never freezes through a minute rollover

## Setup

### 1. Install dependencies

Install the following libraries via the Arduino Library Manager:

- `Adafruit GFX Library`
- `Adafruit LED Backpack Library`
- `RTClib` (by Adafruit)
- `DHT sensor library` (by Adafruit)
- `Adafruit Unified Sensor` (DHT dependency)
- `PubSubClient` (by Nick O'Leary)
- ESP8266 board package (`esp8266:esp8266`) — [install guide](https://arduino-esp8266.readthedocs.io/en/latest/installing.html)

### 2. Configure secrets

Copy `secrets.h.template` to `secrets.h` inside the `esp8266_ntp_clock/` folder and fill in your values:

```bash
cp secrets.h.template esp8266_ntp_clock/secrets.h
```

Edit `esp8266_ntp_clock/secrets.h`:

```cpp
#define SECRET_DEVICE_NAME   "kitchen_clock"      // lowercase + underscores only
#define SECRET_WIFI_SSID     "your_ssid"
#define SECRET_WIFI_PASSWORD "your_password"
#define SECRET_MQTT_HOST     "192.168.1.x"        // IP or hostname of your MQTT broker
#define SECRET_MQTT_USER     "your_mqtt_user"
#define SECRET_MQTT_PASSWORD "your_mqtt_password"  // set to "" if no auth
```

`DEVICE_NAME` drives the MQTT topic paths and HA entity IDs — e.g. `kitchen_clock` publishes to `feather_clock/kitchen_clock/dht_temperature`.

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

## Home Assistant / MQTT

The clock uses [MQTT discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery) to register its sensors automatically — no YAML configuration needed. Ensure the **MQTT integration** is enabled in Home Assistant and pointed at the same broker.

Each publish cycle (every 5 minutes) sends retained discovery configs followed by the current state values:

| MQTT topic | Unit | Description |
|------------|------|-------------|
| `feather_clock/<name>/dht_temperature` | °F | Ambient temperature (DHT22) |
| `feather_clock/<name>/dht_humidity` | % | Relative humidity (DHT22) |
| `feather_clock/<name>/rtc_temperature` | °F | DS3231 on-chip temperature (runs high) |
| `feather_clock/<name>/wifi_rssi` | dBm | WiFi signal strength |
| `feather_clock/<name>/last_ntp_sync` | — | ISO 8601 timestamp of last NTP sync |

Discovery payloads are published to `homeassistant/sensor/<name>_<sensor>/config` with `retain=true`, so HA picks them up after a restart even between report cycles. State topics are also retained so dashboards always show the last known value.
