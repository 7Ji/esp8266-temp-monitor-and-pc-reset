#include <Arduino.h>
#include <DHT.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

static_assert(sizeof(unsigned long) == sizeof(uint32_t), "unsigned long (would return by millis) is not 32 bit");

#include "root.html.h"

#define COMPCONST static inline constexpr

COMPCONST char const ConfigHostName[] = PRIVATE_HOSTNAME;
COMPCONST char const ConfigWiFiSSID[] = PRIVATE_WIFI_SSID;
COMPCONST char const ConfigWiFiPassword[] = PRIVATE_WIFI_PASSWORD;
COMPCONST unsigned long const ConfigBaudRate = 115200;

COMPCONST char const ErrorStringWouldTruncate[] PROGMEM = "String Would Truncate";
COMPCONST char const ErrorStringFormatFailure[] PROGMEM = "String Format Failure";
COMPCONST char const ErrorNotFound[] PROGMEM = "Not Found";
COMPCONST char const PcPowerOk[] PROGMEM = "Power button pulse sent";
COMPCONST char const PcResetOk[] PROGMEM = "Reset button pulse sent";

COMPCONST unsigned long const OneSecondAsMs = 1'000;
COMPCONST uint8_t const MaxWaits = 20;
COMPCONST uint8_t const PcPinPower =
#ifdef PRIVATE_PIN_POWER
PRIVATE_PIN_POWER
#else
D1
#endif
;
COMPCONST uint8_t const PcPinReset =
#ifdef PRIVATE_PIN_RESET
PRIVATE_PIN_RESET
#else
D2
#endif
;
static DHT dht(
#ifdef PRIVATE_PIN_DHT11
PRIVATE_PIN_DHT11
#else
D4
#endif
, DHT11);
static ESP8266WebServer server(80);

#define serverSendError(x) server.send_P(500, "text/plain", Error ## x, sizeof(Error ## x) - 1)

struct NtpSyncer {
  COMPCONST uint32_t const NtpUnixOffset = 2208988800UL;
  COMPCONST uint32_t const MinUnixOffset = 1776133834UL; /* Tue Apr 14 11:00:01 CST 2026, no specific reason */
  COMPCONST uint32_t const MinNtp = NtpUnixOffset + MinUnixOffset;

  COMPCONST char NtpServer[] =
#ifdef PRIVATE_NTP_SERVER
    PRIVATE_NTP_SERVER
#else
    "pool.ntp.org"
#endif
      ;
  COMPCONST uint16_t const UdpPortNtp =
#ifdef PRIVATE_NTP_LOCAL_PORT
    PRIVATE_NTP_LOCAL_PORT
#else
    7931 /* This is chosen simply randomly */
#endif
      ;

  IPAddress serverIP;
  WiFiUDP udp;
  byte buffer[48];
  uint32_t millisLast;
  bool init, isIP;

  uint64_t unixOffset = MinUnixOffset;

  void update(uint32_t const millisCurrent) {
    uint8_t i;
    uint32_t ntpOffset, naiveUnixOffset;

    if (!init) {
      if (!udp.begin(UdpPortNtp)) {
        Serial.printf("Failed to listen on %" PRIu16 "\n", UdpPortNtp);
        return;
      }
      if (serverIP.fromString(NtpServer)) {
        isIP = true;
      }
      init = true;
    }
    if (!isIP) {
      if (!WiFi.hostByName(NtpServer, serverIP, 1000)) {
        Serial.printf("Failed to resolve '%s' to IP\n", NtpServer);
        return;
      }
    }
    if (!udp.beginPacket(serverIP, 123)) {
      Serial.println("Failed to begin NTP packet");
      return;
    }
    buffer[0] = 0b11100011;
    memset(buffer + 1, 0, sizeof(buffer) - 1);
    if (udp.write(buffer, sizeof(buffer)) != sizeof(buffer)) {
      Serial.println("Failed to write whole NTP packet");
      return;
    }
    if (!udp.endPacket()) {
      Serial.println("Failed to end UDP packet");
      return;
    }
    for (i = 0;;) {
      if (udp.parsePacket()) {
        break;
      }
      delay(100);
      if (++i > 20) {
        Serial.println("Timeout waiting for NTP response");
        return;
      }
    }
    if (udp.read(buffer, sizeof buffer) != sizeof buffer) {
      Serial.println("Failed to read NTP response");
      return;
    }
    ntpOffset = ((uint32_t(word(buffer[40], buffer[41])) << 16) | word(buffer[42], buffer[43]));

    naiveUnixOffset = ntpOffset - NtpUnixOffset;
    if (ntpOffset < MinNtp) {
      Serial.printf("NTP offset %" PRIu32 " < minimum NTP %" PRIu32 ", adding one era\n", ntpOffset, MinNtp);
      unixOffset = 0x100000000ULL + naiveUnixOffset;
    } else {
      unixOffset = naiveUnixOffset;
    }
    unixOffset -= millisCurrent / 1000;
    Serial.printf("Current unix offset is %" PRIu64 "\n", unixOffset);
    millisLast = millisCurrent;
  }

  void firstUpdate(uint32_t const millisCurrent) {
    update(millisCurrent);
  }

  void maybeUpdate(uint32_t const millisCurrent) {
    /* Force update if not initialized yet, or */
    if ((millisCurrent - millisLast) > 3600000UL || !init) {
      update(millisCurrent);
    }
  }
};

static NtpSyncer ntpSyncer = {};

struct SensorRecord {
  COMPCONST int const lenBuffer = 38; /* extreme 18446744073709551615,255.255,255.255\n */
  COMPCONST int const lenBufferShort = 16; /* extreme 255.255,255.255\0 */

  uint32_t timestamp; /* second */
  int8_t tempInt;
  uint8_t tempDot;
  uint8_t humidInt;
  uint8_t humidDot;

  /* buffer should be at least lenBuffer */
  bool intoStr(uint64_t const unixOffset, char *const buffer, size_t &len) const {
    int const r = snprintf(buffer, lenBuffer, "%" PRIu64 ",%" PRId8 ".%" PRIu8 ",%" PRIu8 ".%" PRIu8 "\n", unixOffset + timestamp, tempInt, tempDot, humidInt, humidDot);

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
};

static_assert(sizeof(struct SensorRecord) == 8, "SensorRecord should have a size of 4");

struct SensorSlice {
  COMPCONST uint32_t const Magic = 0x82660C05;
  COMPCONST uint16_t const MaxRecords = 30;
  COMPCONST uint16_t const MaxRecordsSub1 = MaxRecords - 1;
  COMPCONST uint16_t const Count32 = (MaxRecords + 1) * 2;

  uint32_t magic = Magic;
  uint32_t checksum;
  uint64_t unixOffset;
  SensorRecord records[MaxRecords];

  uint32_t actualChecksum() {
    uint32_t const *const raw = reinterpret_cast<uint32_t const *>(&unixOffset);
    uint32_t x = *raw;
    uint16_t i;
    for (i = 1; i < Count32; ++i) {
      x ^= raw[i];
    }
    return x;
  }

  void shift() {
    uint16_t recordID, recordIDNext;

    for (recordID = 0; recordID < MaxRecordsSub1;) {
      recordIDNext = recordID + 1;
      records[recordID] = records[recordIDNext];
      recordID = recordIDNext;
    }
  }
};

struct SensorHistory {
  COMPCONST uint8_t const MaxHistoryL0 = 1 << 5; /* 2s * 32, even secondly, for about a minute */

  COMPCONST uint8_t const RingMaskL0 = MaxHistoryL0 - 1;

  COMPCONST uint32_t const FlashAddrStart = 0x100000;
  COMPCONST uint32_t const FlashAddrEnd = 0x3FB000;
  COMPCONST int const FlashSectExp = 12;
  COMPCONST uint16_t const FlashSectSize = 1 << FlashSectExp; /* 4096 */
  COMPCONST uint16_t const FlashSectStart = FlashAddrStart / FlashSectSize;
  COMPCONST uint16_t const FlashSectEnd = FlashAddrEnd / FlashSectSize;
  COMPCONST uint16_t const FlashSectCount = FlashSectEnd - FlashSectStart;
  COMPCONST int const FlashSectPageFactor = 4;
  COMPCONST uint8_t const FlashSectPageCount = 1 << FlashSectPageFactor;
  COMPCONST uint16_t const FlashPageSize = 1 << (FlashSectExp - FlashSectPageFactor); /* 256 */
  static_assert(sizeof(struct SensorSlice) == FlashPageSize, "SensorSlice should have the same size as page");
  COMPCONST uint16_t const FlashPageInSectMask = FlashSectPageCount - 1;
  COMPCONST uint16_t const FlashPageStart = FlashAddrStart / FlashPageSize;
  COMPCONST uint16_t const FlashPageEnd = FlashAddrEnd / FlashPageSize;
  COMPCONST uint16_t const FlashPageCount = FlashPageEnd - FlashPageStart;
  COMPCONST uint16_t const FlashPageCountSubSect = FlashPageCount - FlashSectPageCount;

  union {
    SensorSlice sliceL1 = {.magic = SensorSlice::Magic};
    uint32_t sliceL1Raw[sizeof(SensorSlice) / sizeof(uint32_t)];
  };
  union {
    SensorSlice sliceL2; /* about minutely, flush to flash every 30 minutes */
    uint32_t sliceL2Raw[sizeof(SensorSlice) / sizeof(uint32_t)];
  };
  SensorRecord recordsL0[MaxHistoryL0];
  uint32_t secondsLast = 0;
  uint32_t millisLast = 0;
  uint32_t secondsOffset = 0;
  uint32_t millisOffset = 0;
  uint16_t countL2 = 0; /* page */
  uint16_t headL2 = 0; /* sector */
  uint16_t countL1 = 0;
  uint8_t headL0 = 0;
  uint8_t countL0 = 0;

  SensorRecord &first() {
    if (countL2 > 0 && fetchL2(headL2)) {
      return sliceL2.records[0];
    } else if (countL1 > 0) {
      return sliceL1.records[0];
    } else {
      return recordsL0[headL0];
    }
  }

  SensorRecord &last() {
    return recordsL0[(headL0 + countL0 - 1) & RingMaskL0];
  }

  bool erase(uint16_t const sectorID) {
    uint16_t const flashSectorID = sectorID + FlashSectStart;
    SpiFlashOpResult opResult;

    Serial.printf("Erasing sector %" PRIu16 " /f%" PRIu16 "\n", sectorID, flashSectorID);
    opResult = spi_flash_erase_sector(flashSectorID);
    if (opResult != SPI_FLASH_RESULT_OK) {
      Serial.printf("Failed to erase sector %" PRIu16 "/f%" PRIu16 ", op result %d\n", sectorID, flashSectorID, opResult);
      return false;
    }
    return true;
  }

  bool writeL1(uint16_t const pageID) {
    uint16_t const flashPageID = pageID + FlashPageStart;
    SpiFlashOpResult opResult;

    Serial.printf("Writing L1 to page %" PRIu16 " /f%" PRIu16 "\n", pageID, flashPageID);
    opResult = spi_flash_write(flashPageID * FlashPageSize, sliceL1Raw, FlashPageSize);
    if (opResult != SPI_FLASH_RESULT_OK) {
      Serial.printf("Failed to write L1 to page %" PRIu16 " /f%" PRIu16 ", op result %d\n", pageID, flashPageID, opResult);
      return false;
    }
    return true;
  }

  void fallbackShift() {
    Serial.println("Falling back to shift L1 by one position");
    sliceL1.shift();
    countL1 = SensorSlice::MaxRecordsSub1;
  }

  void flushL1L2() {
    uint16_t const pageID = ((headL2 << FlashSectPageFactor) + countL2) % FlashPageCount;
    bool const afterBoundary = pageID & FlashPageInSectMask;

    Serial.printf("Flushing L1 to L2 flash page %" PRIu16 "\n", pageID);

    if (!afterBoundary) {
      /* Whether we successfully erase the sector or not, consider pages on it bad */
      if (countL2 > FlashPageCountSubSect) { /* Technically this could only be countL2 == FlashPageCount, but do this securely */
        headL2 = (headL2 + 1) % FlashSectCount;
        countL2 = FlashPageCountSubSect;
      }
      if (!erase(pageID >> FlashSectPageFactor)) {
        fallbackShift();
        return;
      }
    }
    sliceL1.unixOffset = ntpSyncer.unixOffset;
    sliceL1.checksum = sliceL1.actualChecksum();
    if (!writeL1(pageID)) {
      fallbackShift();
      return;
    }
    countL1 = 0;
    ++countL2;
  }

  bool fetchL2(uint16_t const pageID) {
    uint16_t const flashPageID = pageID + FlashPageStart;
    uint32_t checksum;
    SpiFlashOpResult opResult;

    opResult = spi_flash_read(flashPageID * FlashPageSize, sliceL2Raw, FlashPageSize);

    if (opResult != SPI_FLASH_RESULT_OK) {
      Serial.printf("fetch %" PRIu16 "/f%" PRIu16 ": Read not OK, op result %d\n", pageID, flashPageID, opResult);
      return false;
    }
    if (sliceL2.magic != SensorSlice::Magic) {
      Serial.printf("fetch %" PRIu16 "/f%" PRIu16 ": Magic not right (recorded %08" PRIx32 " != expected %08" PRIx32 ")\n", pageID, flashPageID, sliceL2.magic, SensorSlice::Magic);
      return false;
    }
    checksum = sliceL2.actualChecksum();
    if (checksum != sliceL2.checksum) {
      Serial.printf("fetch %" PRIu16 "/f%" PRIu16 ": Checksum mismatch, expected %016" PRIx16 ", found %016" PRIx16  "\n", pageID, flashPageID, checksum, sliceL2.checksum);
      return false;
    }
    return true;
  }

  void recoverFlash() {
    uint16_t pageID, sectorID = 0;
    uint64_t unixLast = 0, unixThis, unixFirst = 0;
    bool first = true;

    /* recover slices */
    Serial.println("Recovering on-flash records...");
    for (pageID = 0; pageID < FlashPageCount; ++pageID) {
      if (!fetchL2(pageID)) {
        if (sectorID > 0) {
          Serial.printf("Failed to fetch uncorrutped L2 at page %" PRIu16 " and we had jumped back at sector %" PRIu16 ", use pages before jump back\n", pageID, sectorID);
          countL2 = sectorID << FlashSectPageFactor;
        } else {
          Serial.printf("Failed to fetch uncorrutped L2 at page %" PRIu16 ", use pages before it\n", pageID);
          countL2 = pageID;
        }
        headL2 = 0;
        return;
      }
      unixThis = sliceL2.unixOffset + sliceL2.records[0].timestamp;
      if (first) {
        unixFirst = unixThis;
        first = false;
      }
      if (unixThis <= unixLast) { /* Jumping back, allowed only once */
        if (sectorID > 0) { /* Already go back once, not possible */
          Serial.printf("Flash page %" PRIu16 " jump back when we already have sector %" PRIu16 " jump back, impossible, use pages before first jump back\n", pageID, sectorID);
          headL2 = 0;
          countL2 = sectorID << FlashSectPageFactor;
          return;
        } else if (pageID & FlashPageInSectMask) {
          Serial.printf("Flash page %" PRIu16 " jump back inside sector, impossible, use pages before it\n", pageID);
          headL2 = 0;
          countL2 = pageID;
          return;
        }
        /* First time jumping back */
        sectorID = pageID >> FlashSectPageFactor;
      } /* else, Most common case, going upward */
      unixLast = unixThis;
    }
    if (sectorID > 0 && unixFirst <= unixLast) {
      Serial.printf("Flash jumped back at sector %" PRIu16 " yet the first unix offset %" PRIu64 " is not larger than last offset %" PRIu64 ", use pages before jumping back\n", sectorID, unixFirst, unixLast);
      headL2 = 0;
      countL2 = sectorID << FlashSectPageFactor;
    } else {
      headL2 = sectorID;
      countL2 = FlashPageCount;
    }
  }

  void fetchAppend(uint32_t const secondsCurrent, uint32_t const millisCurrent) {
    struct SensorRecord * record;
    uint16_t current;

    if (!dht.read(true)) {
      return;
    }

    if (countL0 == MaxHistoryL0) {
      if (!headL0) {
        if (countL1 == SensorSlice::MaxRecords) {
          flushL1L2();
        }
        sliceL1.records[countL1++] = recordsL0[0];
      }
      current = headL0;
      headL0 = (headL0 + 1) & RingMaskL0;
    } else {
      current = (headL0 + countL0++) & RingMaskL0;
    }

    record = recordsL0 + current;

    millisLast = millisCurrent;
    record->timestamp = secondsLast = secondsCurrent;
    record->humidInt = dht.data[0];
    record->humidDot = dht.data[1];
    record->tempInt =  static_cast<int8_t>(dht.data[2]);
    record->tempDot = dht.data[3];
  }

  void firstFetchAppend(uint32_t const millisCurrent) {
    dht.begin();
    fetchAppend(millisCurrent / 1000, millisCurrent);
  }

  void maybeFetchAppend(uint32_t const millisCurrent) {
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

static SensorHistory history = {};

void httpHandleLast() {
  int i, r, offset, remain;
  int const countArgs = server.args();
  SensorRecord &record = history.last();
  char buffer[SensorRecord::lenBufferShort];

  if (countArgs > 0) {
    offset = 0, remain = SensorRecord::lenBufferShort;
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
    r = snprintf(buffer, SensorRecord::lenBufferShort, "%" PRId8 ".%" PRIu8 ",%" PRIu8 ".%" PRIu8 "", record.tempInt, record.tempDot, record.humidInt, record.humidDot);
    if (r < 0) {
      serverSendError(StringFormatFailure);
      return;
    }
    if (r >= SensorRecord::lenBufferShort) {
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

struct RawAllSender {
  COMPCONST size_t const lenBuffer = 1024;
  COMPCONST size_t const lenMax = lenBuffer - SensorRecord::lenBuffer;

  char buffer[lenBuffer];
  size_t len;
  size_t offset = 0;

  RawAllSender() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
#ifdef PRIVATE_ALLOW_CROSS
    serverSendHeadersForRaw();
#endif
    server.send(200, "text/plain", "", 0);
  }

  void sendRecord(uint64_t const unixOffset, SensorRecord const &record) {
    if (record.intoStr(unixOffset, buffer + offset, len)) {
      if ((offset += len) >= lenMax) {
        server.sendContent(buffer, offset);
        offset = 0;
      }
    }
  }

  void sendRingRecords(SensorRecord const *records, uint8_t const head, uint8_t const count) {
    uint64_t const unixOffset = ntpSyncer.unixOffset;
    uint8_t i;

    for (i = head; i < count; ++i) {
      sendRecord(unixOffset, records[i]);
    }
    for (i = 0; i < head; ++i) {
      sendRecord(unixOffset, records[i]);
    }
  }

  void sendPartialSlice(SensorSlice const &slice, uint16_t const count) {
    uint64_t const unixOffset = ntpSyncer.unixOffset;
    uint16_t i;

    for (i = 0; i < count; ++i) {
      sendRecord(unixOffset, slice.records[i]);
    }
  }

  void sendFullSlice(SensorSlice const &slice) {
    uint64_t const unixOffset = slice.unixOffset;
    uint16_t i;

    /* maybe this would use immediate number in instruction, so dont call sendPartialSlice lazily */
    for (i = 0; i < SensorSlice::MaxRecords; ++i) {
      sendRecord(unixOffset, slice.records[i]);
    }
  }

  ~RawAllSender() {
    if (offset) {
      server.sendContent(buffer, offset);
    }
    server.sendContent("", 0);
  }
};

void httpHandleRawAll() {
  uint16_t i;

  RawAllSender sender = {};

  if (history.countL2 > 0) {
    for (i = history.headL2; i < history.countL2; ++i) {
      if (!history.fetchL2(i)) {
        continue;
      }
      sender.sendFullSlice(history.sliceL2);
    }
    for (i = 0; i < history.headL2; ++i) {
      if (!history.fetchL2(i)) {
        continue;
      }
      sender.sendFullSlice(history.sliceL2);
    }
  }
  if (history.countL1 > 0) {
    sender.sendPartialSlice(history.sliceL1, history.countL1);
  }
  if (history.countL0 > 0) {
    sender.sendRingRecords(history.recordsL0, history.headL0, history.countL0);
  }
}

void httpHandleRawLast() {
  char buffer[SensorRecord::lenBuffer];
  size_t len;

  if (history.last().intoStr(ntpSyncer.unixOffset, buffer, len)) {
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
  static String const wantedSSID = String(ConfigWiFiSSID);

  bool recoverFlash = false;
  int8_t countNetworks, bestScanID;
  uint8_t i, *currentBSSID;
  int32_t bestRSSI, currentRSSI, currentChannel;
  uint32_t millisCurrent;
  String currentSSID;

  pinMode(PcPinPower, OUTPUT);
  digitalWrite(PcPinPower, LOW);
  pinMode(PcPinReset, OUTPUT);
  digitalWrite(PcPinReset, LOW);

  Serial.begin(ConfigBaudRate);
  WiFi.setHostname(ConfigHostName);
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
    WiFi.begin(ConfigWiFiSSID, ConfigWiFiPassword, WiFi.channel(bestScanID), WiFi.BSSID(bestScanID));
    WiFi.scanDelete();

    if (!recoverFlash) {
      /* This might be slow so do it before waiting for online */
      history.recoverFlash();
      if (history.countL2 > 0) {
        Serial.printf("Recovered %" PRIu16 " slices from flash, head is %" PRIu16 "\n", history.countL2, history.headL2);
      }
      recoverFlash = true;
    }
    for (i = 0; i < MaxWaits; ++i) {
      if (WiFi.status() == WL_CONNECTED) {
        break;
      }
      delay(500);
    }
    if (i < MaxWaits) {
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

  millisCurrent = millis();
  ntpSyncer.firstUpdate(millisCurrent);
  history.firstFetchAppend(millisCurrent);

  server.on("/", HTTP_GET, httpHandleRoot);
  server.on("/last", HTTP_GET, httpHandleLast);
  server.on("/pc/power", HTTP_POST, httpHandlePcPower);
  server.on("/pc/reset", HTTP_POST, httpHandlePcReset);
  server.on("/raw/last", HTTP_GET, httpHandleRawLast);
  server.on("/raw/all", HTTP_GET, httpHandleRawAll);
  server.onNotFound(httpHandleNotFound);
  server.begin();

  Serial.println("HTTP Server started");
}

void loop() {
  uint32_t millisCurrent;

  server.handleClient();
  millisCurrent = millis();
  ntpSyncer.maybeUpdate(millisCurrent);
  history.maybeFetchAppend(millisCurrent);
}
