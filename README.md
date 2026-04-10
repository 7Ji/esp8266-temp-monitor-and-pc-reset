# ESP8266 Temp Monitor and PC Reset

ESP8266 firmware for two related jobs:

- serving temperature and humidity readings from a DHT11 sensor over Wi-Fi
- eventually emulating a PC power/reset button through a hidden HTTP route

The monitoring part is implemented today. The PC button emulation route is planned, but not implemented yet because the required electronic parts are still missing (just bought them :>).

## Features

- Connects to a configured Wi-Fi network and exposes a small HTTP server
- Reads temperature and humidity from a DHT11 sensor
- Stores sensor history in RAM with multiple retention levels
- Serves a built-in web UI with current readings and history graphs
- Exposes simple text endpoints for automation or external dashboards

## Hardware

- ESP8266 board (`nodemcuv2` target in PlatformIO)
- DHT11 sensor

By default the DHT11 data pin is `D4`. You can override it in `private.ini`.

## Project Layout

- `src/main.cpp`: firmware entry point, sensor sampling, Wi-Fi, and HTTP routes
- `src/root.html`: embedded web UI
- `src/root.html.h.py`: build-time script that converts `root.html` into a C++ header
- `platformio.ini`: PlatformIO project configuration
- `private.ini.example`: template for private build settings
- `lib/DHT sensor library`: the DHT library but with `data[5]` member moved from private to public scope for access with higher efficiency.

## Setup

This project expects a private configuration file before building.

1. Copy `private.ini.example` to `private.ini`.
2. Fill in your real values.

Example:

```ini
[common]
build_flags =
    -DPRIVATE_HOSTNAME="esp8266-monitor"
    -DPRIVATE_WIFI_SSID="Your WiFi AP Name"
    -DPRIVATE_WIFI_PASSWORD="Your WiFi Password"
    -DPRIVATE_PIN_DHT11=D4
```

The build includes `private.ini` through `platformio.ini`, so the firmware will not build correctly until that file exists and is populated.

An optional flag `-DPRIVATE_ALLOW_CROSS` could also be set so `/raw/last` and `/raw/all` routes allow cross-site access, but this adds up the size of each response and is only recommended for debugging purpose.

## Build

With PlatformIO installed:

```bash
platformio run
```

To upload:

```bash
platformio run --target upload
```

To open the serial monitor:

```bash
platformio device monitor
```

## Data persistence

- The hostname, WiFi SSID, WiFi password and pin is hardcoded in firmware once built and flashed.
- The sensors history is stored in a layed ring buffer in RAM, not flash, which means the data does not persist across reboots; the data points are:
  - layer 0: 32 x every 2s (about even secondly)
  - layer 1: 64 x every 64s (about minutely)
  - layer 2: 4096 x every 4096s (about hourly)
  - so the oldest data is about 194 days.
  - the web UI (see below) stores its own history which is not limited to these layer rules, you may want to keep a web page (i.e. as PWA) running as your monitor so you could have more detailed data points.

## Web Interface and API

After boot, the firmware scans for the configured SSID, connects, prints the assigned IP address on the serial console, and starts an HTTP server on port `80`.

Current routes:

- `/`: embedded dashboard with current readings and history graphs
- `/last`: latest reading as plain text, format `temp,humidity`
- `/last?temp`: only the latest temperature
- `/last?humid`: only the latest humidity
- `/raw/last`: latest reading as `timestamp,temp,humidity`
- `/raw/all`: full available history, one sample per line as `timestamp,temp,humidity`

Notes:

- timestamps are based on device uptime in milliseconds
- the web UI reads `/raw/last` and `/raw/all`
- only when `-DPRIVATE_ALLOW_CROSS` flag was set, `/raw/last` and `/raw/all` send permissive CORS and `Cache-Control: no-store`, otherwise (default) CORS is disallowed and you must use the web UI on the board itself.
- when `-DPRIVATE_ALLOW_CROSS` flag was set, it is also possible to run the web UI on a different server or even locally, e.g. `firefox src/root.html?apiBase=http://your-esp-8266.lan`

## Planned PC Control Route

The long-term plan is to add a hidden route that will emulate a PC power or reset button through external electronics connected to the ESP8266. That part is intentionally not present in the current firmware yet.

Until the hardware is available and implemented, this repository should be treated as a temperature and humidity monitor only.

## License

This project is licensed under the GNU Affero General Public License v3.0.

See [`LICENSE`](LICENSE).
