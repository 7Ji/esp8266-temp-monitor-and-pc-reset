#include <Arduino.h>
#include <DHT.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>

#define OneSecondAsMs 1'000
#define BaudRate 115200

char const HostName[] = PRIVATE_HOSTNAME;
char const WiFiSSID[] = PRIVATE_WIFI_SSID;
char const WiFiPassword[] = PRIVATE_WIFI_PASSWORD;

#include "root.html.h"

#ifdef PRIVATE_PIN_DHT11
DHT dht(PRIVATE_PIN_DHT11, DHT11);
#else
DHT dht(D4, DHT11);
#endif
ESP8266WebServer server(80);

char const ErrorStringWouldTruncate[] PROGMEM = "String Would Truncate";
char const ErrorStringFormatFailure[] PROGMEM = "String Format Failure";
char const ErrorNotFound[] PROGMEM = "Not Found";

#define serverSendError(x) server.send_P(500, "text/plain", Error ## x, sizeof(Error ## x) - 1)

struct SensorRecord {
  static int const lenBuffer = 32;
  typedef char (&BufferT)[lenBuffer];

  uint32_t timestamp;
  uint8_t tempInt;
  uint8_t tempDot;
  uint8_t humidInt;
  uint8_t humidDot;

  bool intoStr(BufferT buffer, size_t &len) {
    int const r = snprintf(buffer, lenBuffer, "%" PRIu32 ",%" PRIu8 ".%" PRIu8 ",%" PRIu8 ".%" PRIu8 "\n", timestamp, tempInt, tempDot, humidInt, humidDot);

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
    int const r = snprintf(buffer, lenBuffer, "%" PRIu32 ",%" PRIu8 ".%" PRIu8 ",%" PRIu8 ".%" PRIu8 "\n", timestamp, tempInt, tempDot, humidInt, humidDot);

    return r > 0 && r < lenBuffer;
  }
};

struct SensorHistory {
  static int const MaxHistoryExpL0 = 5; /* 2s * 32, even secondly, for about a minute */
  static int const MaxHistoryExpL1 = 6; /* 64s * 64, minutely, for about an hour */
  static int const MaxHistoryExpL2 = 12; /* 4096s * 4096, hourly, for about 194 days */

  static uint8_t const MaxHistoryL0 = 1 << MaxHistoryExpL0;
  static uint8_t const MaxHistoryL1 = 1 << MaxHistoryExpL1;
  static uint16_t const MaxHistoryL2 = 1 << MaxHistoryExpL2;

  static uint8_t const RingMaskL0 = MaxHistoryL0 - 1;
  static uint8_t const RingMaskL1 = MaxHistoryL1 - 1;
  static uint16_t const RingMaskL2 = MaxHistoryL2 - 1;

  SensorRecord entriesL2[MaxHistoryL2];
  SensorRecord entriesL1[MaxHistoryL1];
  SensorRecord entriesL0[MaxHistoryL0];
  uint32_t timeLast = 0;
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

  void fetchAppend(unsigned long const timestamp) {
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
              entriesL2[headL2] = entriesL0[0];
              headL2 = (headL2 + 1) & RingMaskL2;
            } else {
              entriesL2[(headL2 + countL2++) & RingMaskL2] = entriesL0[0];
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

    record->timestamp = timeLast = timestamp;
    record->humidInt = dht.data[0];
    record->humidDot = dht.data[1];
    record->tempInt = dht.data[2];
    record->tempDot = dht.data[3];
  }

  void maybeFetchAppend() {
    unsigned long timeCurrent = millis();

    if (timeCurrent - timeLast <= 2000) {
      return;
    }
    fetchAppend(timeCurrent);
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
        r = snprintf(buffer + offset, remain, ",%" PRIu8 ".%" PRIu8, record.tempInt, record.tempDot);
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
    r = snprintf(buffer, lenBuffer, "%" PRIu8 ".%" PRIu8 ",%" PRIu8 ".%" PRIu8 "", record.tempInt, record.tempDot, record.humidInt, record.humidDot);
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
  int8_t countNetworks, i, bestScanID;
  int8_t const maxWaits = 20; /* half second each, so 10 seconds in total */
  uint8_t *currentBSSID;
  int32_t bestRSSI, currentRSSI, currentChannel;
  String const wantedSSID = String(WiFiSSID);
  String currentSSID;

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
  server.on("/raw/last", HTTP_GET, httpHandleRawLast);
  server.on("/raw/all", HTTP_GET, httpHandleRawAll);
  server.onNotFound(httpHandleNotFound);
  server.begin();

  history.fetchAppend(millis());

  Serial.println("HTTP Server started");
}

void loop() {
  server.handleClient();
  history.maybeFetchAppend();
}
