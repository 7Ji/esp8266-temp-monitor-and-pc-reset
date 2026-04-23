# ESP8266 Temp Monitor and PC Reset

## Features

- Reads temperature and humidity from a DHT11 sensor
- Stores sensor history in layered RAM and flash buffers
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

With PlatformIO installed, build:

```sh
pio run
```

To test (algorithm on host):

```sh
pio test -e native -v
```

To upload:

```sh
pio run --target upload
```

To open the serial monitor:

```sh
pio device monitor
```

## Data Persistence

The hostname, WiFi SSID, WiFi password, pins and auth token are hardcoded in firmware once built and flashed. Sensor history uses a layered buffer so recent data stays detailed while older data is decimated and persisted to flash.

- `L0`: RAM ring, 32 sensor values sampled every 2 seconds, covering about 64 seconds.
- `L0` also keeps a parallel absolute `unixSeconds` array in RAM so each live sample carries its capture-time Unix timestamp.
- `L1`: RAM page, 40 records promoted from `L0` every 32 samples, so one promoted record represents about 64 seconds and one full page covers about 42.7 minutes.
- `L2`: flash ring, same 256-byte `SensorPage` format as `L1`, written one full `L1` page per flash page.

The `L2` flash area starts at `0x100000` and ends at `0x3FB000`, matching the `eagle.flash.4m.ld` layout that leaves the top flash sectors for EEPROM, RF calibration and WiFi data. This gives `12208` flash pages across `763` sectors. The writer advances at page granularity, but erases at 4 KiB sector granularity. When the ring wraps to the first page of the current head sector, it advances `headL2` by one sector, erases the target sector, writes the first new page, then fills the rest of that sector page-by-page.

After the flash ring is full, retained `L2` capacity ranges from `12193` pages just after a sector rollover to the full `12208` pages just before the next rollover. At the current cadence, the full ring holds `12208 * 40 = 488320` decimated records. Since each retained record represents about 64 seconds, the oldest persisted data is about 362 days old. A sector is erased only when advancing to a new 16-page sector, about every 10.7 hours of samples, and a full cycle across all sectors is also about 362 days, so flash wear is intentionally low.

## On-Flash Format and Recovery

Each flash page stores one `SensorPage`:

```text
page[256]
  0x000..0x003  magic        (u32)
  0x004..0x007  checksum     (u32)
  0x008..0x00f  unixOffset   (u64)
  0x010..0x0ff  records[40]  (40 x 6 bytes)

record[6]
  0x0..0x1  timestamp  (u16)
  0x2..0x5  value      (SensorValue)

value[4]
  0x0       tempInt    (i8)
  0x1       tempDot    (u8)
  0x2       humidInt   (u8)
  0x3       humidDot   (u8)
```

The page is exactly 256 bytes, matching the ESP8266 flash page write size. `timestamp` is the per-record second offset from the page `unixOffset`, with the first record in each page always stored as offset `0`. `L0` keeps only `SensorValue` entries plus the parallel absolute Unix timestamp array; `SensorRecord` is used by `L1` and `L2` where compact per-page offsets are useful. `checksum` is CRC32 over `unixOffset` and the full records array.

On boot, recovery scans the whole `L2` flash range sector-by-sector, reading one full 4 KiB sector at a time into the shared static buffer and then inspecting its 16 pages in memory. Empty pages are treated as erased space, and all-empty sectors are skipped, so valid history can be recovered even when it starts at a later sector instead of at the beginning of flash. Inside one candidate chain, pages must be contiguous and ordered by `unixOffset`, which acts as the page's implicit monotonic identifier. In this firmware that is a practical ordering key because each page is written about every 42.7 minutes and `unixOffset` is not allowed to move backwards.

If a sector read fails, a written page is invalid, a non-empty page appears after an empty page in the same sector, or `unixOffset` goes backwards, recovery closes the current candidate and continues scanning from the next recoverable sector. A backwards jump on the first page of a sector starts a new candidate at that sector, matching the sector-boundary ring-wrap behavior of the writer. After the scan, recovery chooses the candidate with the newest end timestamp, using the final record timestamp within the candidate's last page instead of only that page's base `unixOffset`. As a special case, if one candidate starts at the beginning of flash and another ends exactly at the end of flash, recovery joins them as a wrapped ring only when the beginning candidate is newer than the tail candidate; otherwise it keeps the newer candidate. This supports orphaned valid chunks in the middle of flash as well as the usual fresh chain and wrapped-ring layouts.

## Binary Dump Format

The `/dump` route returns all available history as `application/octet-stream`. Multi-byte integer fields are little-endian.

```text
dump_header[16]
  0x00..0x02  magic       "E82" (bytes 0x45 0x38 0x32)
  0x03        version     u8, currently 1
  0x04..0x07  count       u32le, number of records following the header
  0x08..0x0f  unixOffset  u64le, base Unix timestamp in seconds

dump_record[8] repeated count times
  0x00..0x03  timestamp   u32le, seconds after header unixOffset
  0x04        tempInt     i8, integer part of temperature in Celsius
  0x05        tempDot     u8, first decimal digit of temperature
  0x06        humidInt    u8, integer part of relative humidity percentage
  0x07        humidDot    u8, first decimal digit of relative humidity
```

The absolute Unix timestamp for each record is `header.unixOffset + record.timestamp`. The temperature value is `tempInt + tempDot / 10`; the humidity value is `humidInt + humidDot / 10`. Consumers should reject dumps shorter than 16 bytes, dumps with unsupported magic or version, and dumps whose byte length is not exactly `16 + count * 8`.

## Web Interface and API

After boot, the firmware scans for the configured SSID, connects, prints the assigned IP address on the serial console, and starts an HTTP server on port `80`.

Current routes:

- `/`: embedded dashboard with current readings and history graphs
- `/last`: latest reading as plain text, intended to be used by scripts, format `temp,humidity`
- `/temp`: only the latest temperature
- `/humid`: only the latest humidity
- `/recent`: latest reading as `timestamp,temp,humidity`
- `/history`: full available history, one sample per line as `timestamp,temp,humidity`
- `/dump`: full available history as binary `application/octet-stream`; see Binary Dump Format
- `/power`: authenticated `POST`, sends a short pulse on `PRIVATE_PIN_POWER`; requires header `Authorization: Bearer <PRIVATE_PC_AUTH>` and reply with `404` when unauthorized (to hide the route)
- `/reset`: authenticated `POST`, sends a short pulse on `PRIVATE_PIN_RESET`; requires header `Authorization: Bearer <PRIVATE_PC_AUTH>` and reply with `404` when unauthorized (to hide the route)

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
curl --request POST --url http://[ip-or-host-name]/power --header 'Authorization: Bearer [your key]'
```

For reset:

```sh
curl --request POST --url http://[ip-or-host-name]/reset --header 'Authorization: Bearer [your key]'
```

## License

This project is licensed under the GNU Affero General Public License v3.0.

See [`LICENSE`](LICENSE).
