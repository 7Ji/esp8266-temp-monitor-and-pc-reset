# ESP8266 Temp Monitor and PC Reset

ESP8266 firmware for two related jobs:

- serving temperature and humidity readings from a DHT11 sensor over Wi-Fi
- emulating a PC power/reset button through hidden HTTP routes

## Features

- Connects to a configured Wi-Fi network and exposes a small HTTP server
- Reads temperature and humidity from a DHT11 sensor
- Stores sensor history in RAM with multiple retention levels
- Serves a built-in web UI with current readings and history graphs
- Exposes simple text endpoints for automation or external dashboards
- Exposes authenticated `POST` routes for PC power and reset button pulses

## Hardware

- ESP8266 board (`nodemcuv2` target in PlatformIO)
- DHT11 sensor, on data pin `D4`
- 2 x optocouplers, one for power button (on data pin `D1`) and one for reset button (on data pin `D2`), e.g. `PC817`
- 2 x `680 Ω` ~ `1k Ω` resistors, one for power button and one for reset button, I'm using `680 Ω` ones

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
    -DPRIVATE_PC_AUTH="replace-with-random-bearer-token"
    -DPRIVATE_PIN_POWER=D1
    -DPRIVATE_PIN_RESET=D2
    -DPRIVATE_PIN_DHT11=D4
```

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
- `/pc/power`: authenticated `POST`, sends a short pulse on `PRIVATE_PIN_POWER`; requires header `Authorization: Bearer <PRIVATE_PC_AUTH>` and reply with `404` when unauthorized
- `/pc/reset`: authenticated `POST`, sends a short pulse on `PRIVATE_PIN_RESET`; requires header `Authorization: Bearer <PRIVATE_PC_AUTH>` and reply with `404` when unauthorized

## PC Control Wiring

The PC control routes are intended to drive external isolation hardware, not a motherboard header directly from the ESP8266 GPIO. A typical setup uses one optocoupler per motherboard button input, with the optocoupler transistor placed in parallel with the case button.

Recommended defaults in this repository are:

- `PRIVATE_PIN_POWER=D1`
- `PRIVATE_PIN_RESET=D2`

These map to `GPIO5` and `GPIO4` on common ESP8266 dev boards and are safer choices than boot-strap pins such as `D3`, `D4`, or `D8`.

Use one `PC817` for the motherboard power button header and one `PC817` for the reset button header.

Textual wiring for each button channel:

- ESP8266 `D1` or `D2` -> `680R` to `1k` resistor -> `PC817` pin `1` (LED anode)
- ESP8266 `GND` -> `PC817` pin `2` (LED cathode)
- `PC817` pin `4` (transistor collector) -> motherboard button signal pin
- `PC817` pin `3` (transistor emitter) -> motherboard button `GND`
- The normal case switch can be left connected; the optocoupler could be wired in parallel with it, if you want to.

Do not connect ESP8266 `GND` directly to the motherboard front-panel `GND` when using the optocoupler in this isolated arrangement. The `PC817` provides the isolation between the MCU side and the motherboard side.

Diagram:

```text
Power channel
=============

ESP8266 side                          Motherboard side
-----------                          ----------------

D1 / GPIO5 ---[680R..1k]--- pin 1    pin 4 -------- PWR_SW signal
ESP GND ------------------- pin 2    pin 3 -------- PWR_SW GND
                             PC817


Reset channel
=============

ESP8266 side                          Motherboard side
-----------                          ----------------

D2 / GPIO4 ---[680R..1k]--- pin 1    pin 4 -------- RESET_SW signal
ESP GND ------------------- pin 2    pin 3 -------- RESET_SW GND
                             PC817
```

The firmware pulse is active-high on the ESP8266 pin: when `/pc/power` or `/pc/reset` is called with the correct bearer token, the configured GPIO goes `HIGH` for about `250 ms`, turning on the `PC817` LED and momentarily shorting the corresponding motherboard button input to ground.

A `curl` command shall look like the following:

```sh
curl --request POST --url http://[ip-or-host-name]/pc/power --header 'Authorization: Bearer [your key]'
```

## License

This project is licensed under the GNU Affero General Public License v3.0.

See [`LICENSE`](LICENSE).
