# TC Firmware (ESP‑IDF)

ESP‑IDF firmware that generates GPS + battery telemetry, encodes a 10‑character hex payload, and publishes via MQTT or HTTP at a configurable interval. Uses Wi‑Fi STA and SNTP for timestamps.

- Telemetry: simulated GPS and battery percentage
- Transport: MQTT topic or HTTP POST
- Encoding: 5‑byte payload → 10‑char hex
- Identity: device ID from MAC (e.g., ESP32_XXXXXX)
- Time: SNTP sync for date/time fields

## Quickstart

1) Set up ESP‑IDF (v5.x recommended) and export the environment

2) Select the target for ESP32:

```bash
idf.py set-target esp32
```

3) Configure the project (Wi‑Fi, transport, intervals):

```bash
idf.py menuconfig
```

Under `BuddyNinjaTechnicalChallenge`, set Wi‑Fi SSID/password, choose MQTT or HTTP, set MQTT broker/server URL or HTTP server webhook url and, the send interval.

4) Build, flash, and monitor

```bash
idf.py -p <PORT> build flash monitor
```

Press Ctrl+] to exit the monitor.

## Transport

- MQTT topic: `tc-bn/telemetry/<device_id>` (e.g., `tc-bn/telemetry/ESP32_12ABCD`)
- HTTP POST: to the configured server URL (see Menuconfig). Body is JSON as below.

## Telemetry Format

JSON body sent over MQTT/HTTP:

```json
{
  "id": "ESP32_12ABCD",
  "payload": "0A640B321E",  // 10‑char hex, see Encoding
  "date": "2025-11-07",
  "time": "12:34:56"
}
```

Encoding (10‑char hex = 5 bytes):
- Byte 0: latitude integral part (0–255)
- Byte 1: latitude fractional × 100 (0–99)
- Byte 2: longitude integral part (0–255)
- Byte 3: longitude fractional × 100 (0–99)
- Byte 4: battery percentage integral (0–100)

Implementation references: `main/main.c` (`_encode_payload`, `_create_json_payload`). Device ID from MAC: `main/tc_hal.c`.

## Menuconfig Options

Found under: `BuddyNinjaTechnicalChallenge` (from `main/Kconfig.projbuild`).

- GPS Payload Interval in Seconds (`CONFIG_TC_PAYLOAD_GPS_INTERVAL`)
  - Default: `15`
  - Interval between telemetry sends. Set `3600` for 1‑hour intervals.

- WiFi STA SSID (2.4G) (`CONFIG_TC_WIFI_STA_SSID`)
  - Default: `"your_ssid"`
  - SSID of the 2.4G WiFi network to connect to.

- WiFi STA Password (`CONFIG_TC_WIFI_STA_PASSWORD`)
  - Default: `"your_password"`
  - Password for the WiFi network.

- SNTP Server (`CONFIG_TC_SNTP_SERVER`)
  - Default: `"pool.ntp.org"`
  - Used to sync time for date/time fields.

- Enable MQTT (`CONFIG_TC_MQTT_ENABLED`)
  - Default: `y`
  - Toggle between MQTT (enabled) and HTTP (disabled). The communication protocol can be chosen from this option.

- MQTT Broker URL (`CONFIG_TC_MQTT_ENABLED=y` → `CONFIG_TC_MQTT_BROKER_URL`)
  - Default: `"mqtt://broker.emqx.io:1883"`
  - URI of the MQTT broker. Publishes to `tc-bn/telemetry/<device_id>`.

- HTTP Server URL (`CONFIG_TC_MQTT_ENABLED=n` → `CONFIG_TC_HTTP_SERVER_URL`)
  - Default: `"http://192.168.1.2:8000/injest"`
  - HTTP endpoint for POSTing telemetry JSON when MQTT is disabled.
    If you’re using the cloud app in `tc-cloud/`, its default HTTP path is `/ingest`.
    Change to the IP address of the computer running the `tc-cloud` server.


