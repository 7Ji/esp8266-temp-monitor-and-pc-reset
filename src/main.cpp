#include <Arduino.h>
#include <DHT.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>

static_assert(sizeof(unsigned long) == sizeof(uint32_t), "unsigned long (would return by millis) is not 32 bit");

#include "root.html.h"

static char const ErrorStringWouldTruncate[] PROGMEM = "String Would Truncate";
static char const ErrorStringFormatFailure[] PROGMEM = "String Format Failure";
static char const ErrorNotFound[] PROGMEM = "Not Found";
static char const PcPowerOk[] PROGMEM = "Power button pulse sent";
static char const PcResetOk[] PROGMEM = "Reset button pulse sent";
static uint8_t const PcPinPower =
#ifdef PRIVATE_PIN_POWER
PRIVATE_PIN_POWER
#else
D1
#endif
;
static uint8_t const PcPinReset =
#ifdef PRIVATE_PIN_RESET
PRIVATE_PIN_RESET
#else
D2
#endif
;

#ifdef PRIVATE_PIN_DHT11
DHT dht(PRIVATE_PIN_DHT11, DHT11);
#else
DHT dht(D4, DHT11);
#endif
ESP8266WebServer server(80);

#define serverSendError(x) server.send_P(500, "text/plain", Error ## x, sizeof(Error ## x) - 1)

struct SensorRecord {
  static int const lenBuffer = 32;
  typedef char (&BufferT)[lenBuffer];

  uint32_t timestamp; /* second */
  int8_t tempInt;
  uint8_t tempDot;
  uint8_t humidInt;
  uint8_t humidDot;

  bool intoStr(BufferT buffer, size_t &len) {
    int const r = snprintf(buffer, lenBuffer, "%" PRIu32 ",%" PRId8 ".%" PRIu8 ",%" PRIu8 ".%" PRIu8 "\n", timestamp, tempInt, tempDot, humidInt, humidDot);

    if (r < 0) {
      serverSendError(StringFormatFailure);
      return false;
    } else if (r >= lenBuffer) {
      serverSendError(StringWouldTruncate);
      return false;
    } else {
      len = r;
      return true;
    }
  }

  bool intoStr(BufferT buffer) {
    int const r = snprintf(buffer, lenBuffer, "%" PRIu32 ",%" PRId8 ".%" PRIu8 ",%" PRIu8 ".%" PRIu8 "\n", timestamp, tempInt, tempDot, humidInt, humidDot);

    return r > 0 && r < lenBuffer;
  }
};

struct SensorHistory {
  static uint8_t const MaxHistoryL0 = 1 << 5; /* 2s * 32, even secondly, for about a minute */
  static uint8_t const MaxHistoryL1 = 1 << 6; /* 64s * 64, minutely, for about an hour */
  static uint16_t const MaxHistoryL2 = 1 << 12; /* 4096s * 4096, hourly, for about 194 days */

  static uint8_t const RingMaskL0 = MaxHistoryL0 - 1;
  static uint8_t const RingMaskL1 = MaxHistoryL1 - 1;
  static uint16_t const RingMaskL2 = MaxHistoryL2 - 1;

  SensorRecord entriesL2[MaxHistoryL2];
  SensorRecord entriesL1[MaxHistoryL1];
  SensorRecord entriesL0[MaxHistoryL0];
  uint32_t secondsLast = 0;
  uint32_t millisLast = 0;
  uint32_t secondsOffset = 0;
  uint32_t millisOffset = 0;
  uint16_t headL2 = 0;
  uint16_t countL2 = 0;
  uint8_t headL1 = 0;
  uint8_t countL1 = 0;
  uint8_t headL0 = 0;
  uint8_t countL0 = 0;

  SensorRecord &first() {
    if (countL2 > 0) {
      return entriesL2[headL2];
    } else if (countL1 > 0) {
      return entriesL1[headL1];
    } else {
      return entriesL0[headL0];
    }
  }

  SensorRecord &last() {
    return entriesL0[(headL0 + countL0 - 1) & RingMaskL0];
  }

  SensorRecord &atL0(uint8_t const index) {
    return entriesL0[(headL0 + index) & RingMaskL0];
  }

  SensorRecord &atL1(uint8_t const index) {
    return entriesL1[(headL1 + index) & RingMaskL1];
  }

  SensorRecord &atL2(uint16_t const index) {
    return entriesL2[(headL2 + index) & RingMaskL2];
  }

  SensorRecord &at(uint16_t index) {
    if (index <= countL2) {
      return atL2(index);
    }
    index -= countL2;
    if (index <= countL1) {
      return atL1(index);
    }
    return atL0(index - countL1);
  }

  void fetchAppend(uint32_t const secondsCurrent, uint32_t const millisCurrent) {
    struct SensorRecord * record;
    uint16_t current;

    if (!dht.read(true)) {
      return;
    }

    if (countL0 == MaxHistoryL0) {
      if (!headL0) {
        if (countL1 == MaxHistoryL1) {
          if (!headL1) {
            if (countL2 == MaxHistoryL2) {
              entriesL2[headL2] = entriesL1[0];
              headL2 = (headL2 + 1) & RingMaskL2;
            } else {
              entriesL2[(headL2 + countL2++) & RingMaskL2] = entriesL1[0];
            }
          }
          entriesL1[headL1] = entriesL0[0];
          headL1 = (headL1 + 1) & RingMaskL1;
        } else {
          entriesL1[(headL1 + countL1++) & RingMaskL1] = entriesL0[0];
        }
      }
      current = headL0;
      headL0 = (headL0 + 1) & RingMaskL0;
    } else {
      current = (headL0 + countL0++) & RingMaskL0;
    }

    record = entriesL0 + current;

    millisLast = millisCurrent;
    record->timestamp = secondsLast = secondsCurrent;
    record->humidInt = dht.data[0];
    record->humidDot = dht.data[1];
    record->tempInt =  static_cast<int8_t>(dht.data[2]);
    record->tempDot = dht.data[3];
  }

  void firstFetchAppend() {
    uint32_t const millisCurrent = millis();
    fetchAppend(millisCurrent / 1000, millisCurrent);
  }

  void maybeFetchAppend() {
    uint32_t const millisCurrent = millis();
    uint32_t const millisElasped = millisCurrent - millisLast;

    if (millisElasped >= 2000) {
      if (millisCurrent < millisLast) { /* overflow */
        /* UINT32_MAX = 0xFFFFFFFF = 4294967295 */
        secondsOffset += 4294967;
        millisOffset += 295;
        while (millisOffset >= 1000) {
          millisOffset -= 1000;
          secondsOffset += 1;
        }
      }
      fetchAppend(secondsOffset + (millisCurrent + millisOffset) / 1000, millisCurrent);
    }
  }
};

SensorHistory history = {};

void httpHandleLast() {
  static int const lenBuffer = 16;

  int i, r, offset, remain;
  int const countArgs = server.args();
  SensorRecord &record = history.last();
  char buffer[lenBuffer];

  if (countArgs > 0) {
    offset = 0, remain = lenBuffer;
    buffer[1] = '\0';
    for (i = 0; i < countArgs; ++i) {
      String const &argName = server.argName(i);
      if (argName.startsWith("temp")) {
        r = snprintf(buffer + offset, remain, ",%" PRId8 ".%" PRIu8, record.tempInt, record.tempDot);
      } else if (argName.startsWith("humid")) {
        r = snprintf(buffer + offset, remain, ",%" PRIu8 ".%" PRIu8, record.humidInt, record.humidDot);
      } else {
        continue;
      }
      if (r < 0) {
        serverSendError(StringFormatFailure);
        return;
      }
      if (r >= remain) {
        serverSendError(StringWouldTruncate);
        return;
      }
      offset += r;
      remain -= r;
    }
    server.send(200, "text/plain", buffer + 1, offset - 1);
  } else {
    r = snprintf(buffer, lenBuffer, "%" PRId8 ".%" PRIu8 ",%" PRIu8 ".%" PRIu8 "", record.tempInt, record.tempDot, record.humidInt, record.humidDot);
    if (r < 0) {
      serverSendError(StringFormatFailure);
      return;
    }
    if (r >= lenBuffer) {
      serverSendError(StringWouldTruncate);
    }
    server.send(200, "text/plain", buffer, r);
  }
}

void httpHandleNotFound() {
  server.send_P(404, "text/plain", ErrorNotFound, sizeof(ErrorNotFound) - 1);
}

bool serverIsAuthorized() {
  static String const AuthExpected = String("Bearer " PRIVATE_PC_AUTH);
  static String const HeaderAuthorization = String("Authorization");

  if (!server.hasHeader(HeaderAuthorization)) {
    return false;
  }

  return server.header(HeaderAuthorization) == AuthExpected;
}

void pulseButton(uint8_t const pin) {
  digitalWrite(pin, HIGH);
  delay(250);
  digitalWrite(pin, LOW);
}

void httpHandlePcPower() {
  if (!serverIsAuthorized()) {
    httpHandleNotFound();
    return;
  }

  pulseButton(PcPinPower);
  server.send_P(200, "text/plain", PcPowerOk, sizeof(PcPowerOk) - 1);
}

void httpHandlePcReset() {
  if (!serverIsAuthorized()) {
    httpHandleNotFound();
    return;
  }

  pulseButton(PcPinReset);
  server.send_P(200, "text/plain", PcResetOk, sizeof(PcResetOk) - 1);
}

#ifdef PRIVATE_ALLOW_CROSS
void serverSendHeadersForRaw() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
}
#endif

void httpHandleRawAll() {
  static size_t const lenBuffer = 1024;
  static size_t const lenMax = lenBuffer - SensorRecord::lenBuffer;

  char buffer[lenBuffer];
  union {
    uint16_t i;
    uint8_t j;
  };
  size_t len;
  size_t offset = 0;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
#ifdef PRIVATE_ALLOW_CROSS
  serverSendHeadersForRaw();
#endif
  server.send(200, "text/plain", "", 0);
  for (i = 0; i < history.countL2; ++i) {
    if (history.atL2(i).intoStr(reinterpret_cast<SensorRecord::BufferT>(buffer[offset]), len)) {
      if ((offset += len) >= lenMax) {
        server.sendContent(buffer, offset);
        offset = 0;
      }
    }
  }
  for (j = 0; j < history.countL1; ++j) {
    if (history.atL1(j).intoStr(reinterpret_cast<SensorRecord::BufferT>(buffer[offset]), len)) {
      if ((offset += len) >= lenMax) {
        server.sendContent(buffer, offset);
        offset = 0;
      }
    }
  }
  for (j = 0; j < history.countL0; ++j) {
    if (history.atL0(j).intoStr(reinterpret_cast<SensorRecord::BufferT>(buffer[offset]), len)) {
      if ((offset += len) >= lenMax) {
        server.sendContent(buffer, offset);
        offset = 0;
      }
    }
  }
  if (offset) {
    server.sendContent(buffer, offset);
  }
  server.sendContent("", 0);
}

void httpHandleRawLast() {
  char buffer[SensorRecord::lenBuffer];
  size_t len;

  if (history.last().intoStr(buffer, len)) {
#ifdef PRIVATE_ALLOW_CROSS
    serverSendHeadersForRaw();
#endif
    server.send(200, "text/plain", buffer, len);
  }
}

void httpHandleRoot() {
  server.send_P(200, "text/html; charset=utf-8", RootPage, sizeof(RootPage) - 1);
}

void setup() {
  static char const HostName[] = PRIVATE_HOSTNAME;
  static char const WiFiSSID[] = PRIVATE_WIFI_SSID;
  static char const WiFiPassword[] = PRIVATE_WIFI_PASSWORD;
  static unsigned long const OneSecondAsMs = 1'000;
  static unsigned long const BaudRate = 115200;

  int8_t countNetworks, i, bestScanID;
  int8_t const maxWaits = 20; /* half second each, so 10 seconds in total */
  uint8_t *currentBSSID;
  int32_t bestRSSI, currentRSSI, currentChannel;
  String const wantedSSID = String(WiFiSSID);
  String currentSSID;

  pinMode(PcPinPower, OUTPUT);
  digitalWrite(PcPinPower, LOW);
  pinMode(PcPinReset, OUTPUT);
  digitalWrite(PcPinReset, LOW);

  Serial.begin(BaudRate);
  dht.begin();
  WiFi.setHostname(HostName);
  WiFi.setAutoReconnect(true);

  for (;;) {
    for (;;) {
      Serial.println("Scanning network...");
      countNetworks = WiFi.scanNetworks();
      if (countNetworks > 0) {
        Serial.printf("Discovered %" PRId8 " networks, filtering\n", countNetworks);
        break;
      }
      Serial.println("No network discovered, would rescan after 1 second");
      delay(OneSecondAsMs);
    }

    bestRSSI = INT32_MIN;
    bestScanID = -1;

    for (i = 0; i < countNetworks; ++i) {
      currentSSID = WiFi.SSID(i);
      currentRSSI = WiFi.RSSI(i);
      currentChannel = WiFi.channel(i);
      currentBSSID = WiFi.BSSID(i);
      Serial.printf(
        "[%02" PRId8 "] %-20s (%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8 ") %5" PRId32 " dBm @ ch %" PRId32 "\n",
        i, currentSSID.c_str(),
        currentBSSID[0], currentBSSID[1], currentBSSID[2],
        currentBSSID[3], currentBSSID[4], currentBSSID[5],
        currentRSSI, currentChannel
      );
      if (currentSSID != wantedSSID) {
        continue;
      }
      if (currentRSSI > bestRSSI) {
        if (bestRSSI == INT32_MIN) {
          Serial.println(" => FOUND");
        } else {
          Serial.println(" => REPLACE");
        }
        bestRSSI = currentRSSI;
        bestScanID = i;
      } else {
        Serial.println(" => BYPASS");
      }
    }
    if (bestRSSI == INT32_MIN || bestScanID == -1) {
      Serial.println("Did not find any matching network, would rescan after 1 second");
      WiFi.scanDelete();
      delay(OneSecondAsMs);
      continue;
    }
    Serial.printf("Chose %" PRId8 " as target AP\n", bestScanID);
    WiFi.begin(WiFiSSID, WiFiPassword, WiFi.channel(bestScanID), WiFi.BSSID(bestScanID));
    WiFi.scanDelete();

    for (i = 0; i < maxWaits; ++i) {
      if (WiFi.status() == WL_CONNECTED) {
        break;
      }
      delay(500);
    }
    if (i < maxWaits) {
      Serial.print("WiFi IP: ");
      Serial.print(WiFi.localIP());
      Serial.print(" / ");
      Serial.print(WiFi.subnetMask());
      Serial.print(", Gateway: ");
      Serial.println(WiFi.gatewayIP());
      break;
    }
    Serial.println("Failed to connect, restart scanning process after 1 second");
    delay(OneSecondAsMs);
  }

  server.on("/", HTTP_GET, httpHandleRoot);
  server.on("/last", HTTP_GET, httpHandleLast);
  server.on("/pc/power", HTTP_POST, httpHandlePcPower);
  server.on("/pc/reset", HTTP_POST, httpHandlePcReset);
  server.on("/raw/last", HTTP_GET, httpHandleRawLast);
  server.on("/raw/all", HTTP_GET, httpHandleRawAll);
  server.onNotFound(httpHandleNotFound);
  server.begin();

  history.firstFetchAppend();

  Serial.println("HTTP Server started");
}

void loop() {
  server.handleClient();
  history.maybeFetchAppend();
}
