# ESP8266 Temp Monitor and PC Reset

## Features

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
- `/last`: latest reading as plain text, intended to be used by scripts, format `temp,humidity`
- `/last?temp`: only the latest temperature
- `/last?humid`: only the latest humidity
- `/raw/last`: latest reading as `timestamp,temp,humidity`
- `/raw/all`: full available history, one sample per line as `timestamp,temp,humidity`
- `/pc/power`: authenticated `POST`, sends a short pulse on `PRIVATE_PIN_POWER`; requires header `Authorization: Bearer <PRIVATE_PC_AUTH>` and reply with `404` when unauthorized (to hide the route)
- `/pc/reset`: authenticated `POST`, sends a short pulse on `PRIVATE_PIN_RESET`; requires header `Authorization: Bearer <PRIVATE_PC_AUTH>` and reply with `404` when unauthorized (to hide the route)

## PC Control Wiring

The PC control routes are intended to drive external isolation hardware, not a motherboard header directly from the ESP8266 GPIO. A typical setup uses one optocoupler per motherboard button input, with the optocoupler transistor placed either in parallel with the case button or replacing the real button.

Recommended defaults in this repository are:

- `PRIVATE_PIN_POWER=D1`
- `PRIVATE_PIN_RESET=D2`

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

## PC Control Request

For power on / off:

```sh
curl --request POST --url http://[ip-or-host-name]/pc/power --header 'Authorization: Bearer [your key]'
```

For reset:

```sh
curl --request POST --url http://[ip-or-host-name]/pc/reset --header 'Authorization: Bearer [your key]'
```

## License

This project is licensed under the GNU Affero General Public License v3.0.

See [`LICENSE`](LICENSE).
