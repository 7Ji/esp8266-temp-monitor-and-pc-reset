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

struct UpTimer {
  COMPCONST uint32_t const MillisSafeMax = UINT32_MAX - OneSecondAsMs;

  uint32_t secondsOffset = 0;
  uint32_t millisOffset = 0;
  uint32_t millisLast = 0;

  uint32_t currentMillis() {
    uint32_t const millisCurrent = millis();

    if (millisCurrent < millisLast) { /* overflow */
      /* UINT32_MAX = 0xFFFFFFFF = 4294967295 */
      secondsOffset += 4294967;
      millisOffset += 295;
      while (millisOffset >= OneSecondAsMs) {
        millisOffset -= OneSecondAsMs;
        secondsOffset += 1;
      }
    }
    millisLast = millisCurrent;
    return millisCurrent;
  }

  uint32_t upSeconds(uint32_t const millisCurrent) const {
    return secondsOffset + (millisCurrent + (millisCurrent > MillisSafeMax ? 0 : millisOffset)) / OneSecondAsMs;
  }
};

static UpTimer upTimer = {};

struct NtpSyncer {
  COMPCONST uint32_t const MinInterval = 3600000UL;
  COMPCONST uint32_t const NtpUnixOffset = 2208988800UL;
  COMPCONST uint32_t const MinUnixOffset = 1776133834UL; /* Tue Apr 14 11:00:01 CST 2026, no specific reason */
  COMPCONST uint32_t const MinNtp = NtpUnixOffset + MinUnixOffset;
  COMPCONST size_t const BufferSize = 48;
  COMPCONST uint16_t const NtpPortRemote = 123;
  COMPCONST uint8_t const OffsetSecs = 40;
  COMPCONST uint8_t const OffsetFrac = 44;
  COMPCONST byte const NtpMagic = 0b11100011;

  COMPCONST char NtpServer[] =
#ifdef PRIVATE_NTP_SERVER
    PRIVATE_NTP_SERVER
#else
    "pool.ntp.org"
#endif
      ;
  COMPCONST uint16_t const NtpPortLocal =
#ifdef PRIVATE_NTP_LOCAL_PORT
    PRIVATE_NTP_LOCAL_PORT
#else
    7931 /* This is chosen simply randomly */
#endif
      ;

  IPAddress serverIP;
  WiFiUDP udp;
  byte buffer[BufferSize];
  uint32_t millisLast;
  uint32_t requestToken = 0;
  bool init, isIP;

  uint64_t unixOffset = MinUnixOffset;
  uint64_t unixLast = MinUnixOffset;

  static uint32_t BEu32At(byte const *const data) {
    return (uint32_t(data[0]) << 24) |
           (uint32_t(data[1]) << 16) |
           (uint32_t(data[2]) << 8) |
            uint32_t(data[3]);
  }

  static void BEu32To(byte *const data, uint32_t const value) {
    data[0] = value >> 24;
    data[1] = value >> 16;
    data[2] = value >> 8;
    data[3] = value;
  }

  void discard(size_t size) {
    int discarded;

    while (size > 0) {
      discarded = udp.read(buffer, size > BufferSize ? BufferSize : size);
      if (discarded <= 0 || size_t(discarded) >= size) {
        return;
      }
      size -= discarded;
    }
  }

  void update(uint32_t millisCurrent) {
    uint8_t i;
    uint32_t ntpOffset, requestSecs, requestFrac;
    uint64_t candidateUnixNow;
    int packetSize;

    if (!init) {
      if (!udp.begin(NtpPortLocal)) {
        Serial.printf("Failed to listen on %" PRIu16 "\n", NtpPortLocal);
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
    /* discard pending packets */
    while ((packetSize = udp.parsePacket()) > 0) {
      discard(packetSize);
    }
    if (!udp.beginPacket(serverIP, NtpPortRemote)) {
      Serial.println("Failed to begin NTP packet");
      return;
    }
    buffer[0] = NtpMagic;
    memset(buffer + 1, 0, BufferSize - 1);
    requestSecs = ++requestToken;
    requestFrac = millisCurrent;
    BEu32To(buffer + OffsetSecs, requestSecs);
    BEu32To(buffer + OffsetFrac, requestFrac);
    if (udp.write(buffer, BufferSize) != BufferSize) {
      Serial.println("Failed to write whole NTP packet");
      return;
    }
    if (!udp.endPacket()) {
      Serial.println("Failed to end UDP packet");
      return;
    }
    for (i = 0;;) {
      packetSize = udp.parsePacket();
      if (packetSize > 0) {
        if (size_t(packetSize) < BufferSize) {
          Serial.printf("Ignoring short NTP response of %d bytes\n", packetSize);
          discard(packetSize);
          continue;
        }
        if (udp.read(buffer, BufferSize) != BufferSize) {
          Serial.println("Failed to read NTP response");
          if (size_t(packetSize) > BufferSize) {
            discard(packetSize - BufferSize);
          }
          return;
        }
        if (size_t(packetSize) > BufferSize) {
          discard(packetSize - BufferSize);
        }
        if (udp.remotePort() != NtpPortRemote || udp.remoteIP() != serverIP) {
          Serial.println("Ignoring NTP response from unexpected peer");
          continue;
        }
        if (BEu32At(buffer + 24) != requestSecs || BEu32At(buffer + 28) != requestFrac) {
          Serial.println("Ignoring stale NTP response");
          continue;
        }
        break;
      }
      delay(100);
      if (++i > 20) {
        Serial.println("Timeout waiting for NTP response");
        return;
      }
    }
    millisCurrent = upTimer.currentMillis();
    ntpOffset = BEu32At(buffer + OffsetSecs);

    candidateUnixNow = ntpOffset - NtpUnixOffset;
    if (ntpOffset < MinNtp) {
      Serial.printf("NTP offset %" PRIu32 " < minimum NTP %" PRIu32 ", adding one era\n", ntpOffset, MinNtp);
      candidateUnixNow += 0x100000000ULL;
    }
    if (candidateUnixNow < unixLast) {
      Serial.printf("Ignoring backward unix time %" PRIu64 " < current %" PRIu64 "\n", candidateUnixNow, unixLast);
    } else {
      unixOffset = candidateUnixNow - upTimer.upSeconds(millisCurrent);
      unixLast = candidateUnixNow;
    }
    millisLast = millisCurrent;
    Serial.printf("Current unix time is %" PRIu64 ", unix offset is %" PRIu64 "\n", unixLast, unixOffset);
  }

  void firstUpdate(uint32_t const millisCurrent) {
    update(millisCurrent);
  }

  void observeUnixSeconds(uint64_t const unixSeconds) {
    if (unixSeconds > unixLast) {
      unixLast = unixSeconds;
    }
  }

  uint64_t currentUnixSeconds(uint32_t const millisCurrent) {
    uint64_t const unixSeconds = unixOffset + upTimer.upSeconds(millisCurrent);

    observeUnixSeconds(unixSeconds);
    return unixSeconds;
  }

  void ensureUnixSeconds(uint32_t const millisCurrent, uint64_t const unixSeconds) {
    uint64_t const candidateUnixOffset = unixSeconds - upTimer.upSeconds(millisCurrent);

    if (candidateUnixOffset > unixOffset) {
      Serial.printf("Raising unix offset to recovered floor %" PRIu64 "\n", candidateUnixOffset);
      unixOffset = candidateUnixOffset;
    }
    observeUnixSeconds(unixSeconds);
  }

  void maybeUpdate(uint32_t const millisCurrent) {
    /* Force update if not initialized yet, or */
    if ((millisCurrent - millisLast) >= MinInterval || !init) {
      update(millisCurrent);
    }
  }
};

static NtpSyncer ntpSyncer = {};

struct SensorValue {
  COMPCONST int const lenBuffer = 38; /* extreme 18446744073709551615,255.255,255.255\n */
  COMPCONST int const lenBufferShort = 16; /* extreme 255.255,255.255\0 */

  int8_t tempInt;
  uint8_t tempDot;
  uint8_t humidInt;
  uint8_t humidDot;

  /* buffer should be at least lenBuffer */
  bool intoStr(uint64_t const unixSeconds, char *const buffer, size_t &len) const {
    int const r = snprintf(buffer, lenBuffer, "%" PRIu64 ",%" PRId8 ".%" PRIu8 ",%" PRIu8 ".%" PRIu8 "\n", unixSeconds, tempInt, tempDot, humidInt, humidDot);

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

static_assert(sizeof(struct SensorValue) == 4, "SensorValue should have a size of 4");

struct SensorRecord {
  uint16_t timestamp; /* second offset from slice unixOffset */
  SensorValue value;
};

static_assert(sizeof(struct SensorRecord) == 6, "SensorRecord should have a size of 6");

struct SensorSlice {
  COMPCONST uint32_t const Magic = 0x82660C05;
  COMPCONST uint16_t const MaxRecords = 40;
  COMPCONST uint16_t const MaxRecordsSub1 = MaxRecords - 1;
  COMPCONST uint16_t const CountChecksum32 = (sizeof(uint64_t) + MaxRecords * sizeof(SensorRecord)) / sizeof(uint32_t);
  COMPCONST uint16_t const CountAll32 = CountChecksum32 + 2;

  uint32_t magic = Magic;
  uint32_t checksum;
  uint64_t unixOffset;
  SensorRecord records[MaxRecords];

  uint32_t actualChecksum() const {
    return crc32(&unixOffset, sizeof(unixOffset) + sizeof(records));
  }

  void shift() {
    uint16_t recordID, recordIDNext, diff;

    diff = records[1].timestamp;
    unixOffset += diff;

    for (recordID = 0; recordID < MaxRecordsSub1;) {
      recordIDNext = recordID + 1;
      records[recordID] = records[recordIDNext];
      records[recordID].timestamp -= diff;
      recordID = recordIDNext;
    }
  }

  bool valid() {
    uint32_t expectedChecksum;

    if (magic != SensorSlice::Magic) {
      Serial.printf("Slice magic not right (recorded %08" PRIx32 " != expected %08" PRIx32 ")\n", magic, SensorSlice::Magic);
      return false;
    }
    expectedChecksum = actualChecksum();
    if (checksum != expectedChecksum) {
      Serial.printf("Slice Checksum mismatch (recorded %08" PRIx32 " != expected %08" PRIx32")\n", checksum, expectedChecksum);
      return false;
    }
    return true;
  }
};

struct SensorHistory {
  COMPCONST uint32_t const MinInterval = 2000;
  COMPCONST uint8_t const MaxHistoryL0 = 1 << 5; /* 2s * 32, even secondly, for about a minute */

  COMPCONST uint8_t const RingMaskL0 = MaxHistoryL0 - 1;

  COMPCONST uint32_t const FlashAddrStart = 0x100000;
  COMPCONST uint32_t const FlashAddrEnd = 0x3FB000;
  COMPCONST int const FlashSectExp = 12;
  COMPCONST uint16_t const FlashSectSize = 1 << FlashSectExp; /* 4096 */
  COMPCONST uint16_t const FlashSectStart = FlashAddrStart / FlashSectSize;
  COMPCONST uint16_t const FlashSectEnd = FlashAddrEnd / FlashSectSize;
  COMPCONST uint16_t const FlashSectTotal = FlashSectEnd - FlashSectStart;
  COMPCONST int const FlashPageExp = 8;
  COMPCONST int const FlashSectPageFactor = FlashSectExp - FlashPageExp;
  COMPCONST uint8_t const FlashSectPageCount = 1 << FlashSectPageFactor;
  COMPCONST uint16_t const FlashSectWordCount = FlashSectSize / sizeof(uint32_t);
  COMPCONST uint16_t const FlashPageSize = 1 << FlashPageExp; /* 256 */
  static_assert(sizeof(struct SensorSlice) == FlashPageSize, "SensorSlice should have the same size as page");
  COMPCONST uint16_t const FlashPageInSectMask = FlashSectPageCount - 1;
  COMPCONST uint16_t const FlashPageStart = FlashAddrStart / FlashPageSize;
  COMPCONST uint16_t const FlashPageEnd = FlashAddrEnd / FlashPageSize;
  COMPCONST uint16_t const FlashPageTotal = FlashPageEnd - FlashPageStart;
  COMPCONST uint16_t const FlashPageTotalSubSect = FlashPageTotal - FlashSectPageCount;
  COMPCONST uint8_t const FlashPageWordCount = FlashPageSize / sizeof(uint32_t);

  union {
    SensorSlice sliceL1 = {.magic = SensorSlice::Magic};
    uint32_t sliceL1Raw[sizeof(SensorSlice) / sizeof(uint32_t)];
  };
  union {
    SensorSlice sliceL2; /* about minutely, flush to flash every 30 minutes */
    uint32_t sliceL2Raw[sizeof(SensorSlice) / sizeof(uint32_t)];
  };
  SensorValue valuesL0[MaxHistoryL0];
  uint64_t unixSecondsL0[MaxHistoryL0];
  uint32_t millisLast = 0;
  uint16_t countL2 = 0; /* page */
  uint16_t headL2 = 0; /* sector */
  uint16_t countL1 = 0;
  uint8_t headL0 = 0;
  uint8_t countL0 = 0;

  uint16_t firstPage() const {
    return headL2 << FlashSectPageFactor;
  }

  uint16_t slicePage(uint16_t const sliceID) const {
    return (firstPage() + sliceID) % FlashPageTotal;
  }

  SensorValue &first() {
    if (countL2 > 0 && fetchPage(firstPage())) {
      return sliceL2.records[0].value;
    } else if (countL1 > 0) {
      return sliceL1.records[0].value;
    } else {
      return valuesL0[headL0];
    }
  }

  SensorValue &first(uint64_t &unixSeconds) {
    if (countL2 > 0 && fetchPage(firstPage())) {
      unixSeconds = sliceL2.unixOffset + sliceL2.records[0].timestamp;
      return sliceL2.records[0].value;
    } else if (countL1 > 0) {
      unixSeconds = sliceL1.unixOffset + sliceL1.records[0].timestamp;
      return sliceL1.records[0].value;
    } else {
      unixSeconds = unixSecondsL0[headL0];
      return valuesL0[headL0];
    }
  }

  SensorValue &last() {
    return valuesL0[(headL0 + countL0 - 1) & RingMaskL0];
  }

  SensorValue &last(uint64_t &unixSeconds) {
    uint8_t const recordID = (headL0 + countL0 - 1) & RingMaskL0;

    unixSeconds = unixSecondsL0[recordID];
    return valuesL0[recordID];
  }

  bool erase(uint16_t const sectorID) {
    uint16_t const flashSectorID = sectorID + FlashSectStart;
    SpiFlashOpResult opResult;

    Serial.printf("Erasing sector %" PRIu16 "/f%" PRIu16 "\n", sectorID, flashSectorID);
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

    Serial.printf("Writing L1 to page %" PRIu16 "/f%" PRIu16 "\n", pageID, flashPageID);
    opResult = spi_flash_write(flashPageID * FlashPageSize, sliceL1Raw, FlashPageSize);
    if (opResult != SPI_FLASH_RESULT_OK) {
      Serial.printf("Failed to write L1 to page %" PRIu16 "/f%" PRIu16 ", op result %d\n", pageID, flashPageID, opResult);
      return false;
    }
    return true;
  }

  void fallbackShift() {
    Serial.println("Falling back to shift L1 by one position");
    sliceL1.shift();
    countL1 = SensorSlice::MaxRecordsSub1;
  }

  void appendL1(SensorValue const &valueL0, uint64_t const unixSeconds) {
    uint64_t diff64;
    SensorRecord recordL1 = {.value = valueL0};

    if (countL1 > 0) {
      /* Unlikely to happen, but just in case */
      diff64 = unixSeconds - sliceL1.unixOffset;
      if (diff64 > UINT16_MAX) {
        Serial.printf("Dropping partial L1 with too-wide timestamp delta %" PRIu64 "\n", diff64);
        countL1 = 0;
      }
    }

    if (countL1 == 0) {
      sliceL1.unixOffset = unixSeconds;
      recordL1.timestamp = 0;
    } else {
      recordL1.timestamp = unixSeconds - sliceL1.unixOffset;
    }
    sliceL1.records[countL1++] = recordL1;
  }

  void flushL1L2() {
    uint16_t const pageID = slicePage(countL2);
    bool const afterBoundary = pageID & FlashPageInSectMask;

    Serial.printf("Flushing L1 to L2 flash page %" PRIu16 "\n", pageID);

    if (!afterBoundary) {
      /* Whether we successfully erase the sector or not, consider pages on it bad */
      if (countL2 > FlashPageTotalSubSect) { /* Technically this could only be countL2 == FlashPageTotal, but do this securely */
        headL2 = (headL2 + 1) % FlashSectTotal;
        countL2 = FlashPageTotalSubSect;
      }
      if (!erase(pageID >> FlashSectPageFactor)) {
        fallbackShift();
        return;
      }
    }
    sliceL1.checksum = sliceL1.actualChecksum();
    if (!writeL1(pageID)) {
      fallbackShift();
      return;
    }
    countL1 = 0;
    ++countL2;
  }

  bool erasedL2() {
    uint16_t wordID;

    for (wordID = 0; wordID < SensorSlice::CountAll32; ++ wordID) {
      if (sliceL2Raw[wordID] != UINT32_MAX) {
        return false;
      }
    }
    return true;
  }

  bool fetchFlashPage(uint16_t const flashPageID) {
    SpiFlashOpResult opResult;

    opResult = spi_flash_read(flashPageID * FlashPageSize, sliceL2Raw, FlashPageSize);

    if (opResult != SPI_FLASH_RESULT_OK) {
      Serial.printf("fetch f%" PRIu16 ": Read not OK, op result %d\n", flashPageID, opResult);
      return false;
    }
    return sliceL2.valid();
  }

  bool fetchPage(uint16_t const pageID) {
    return fetchFlashPage(pageID + FlashPageStart);
  }

  void recoverFlash() {
    uint16_t sectorID, flashSectorID, sectorPageID, flashPageID, pageCount = 0, firstPageCount = 0, secondSectHead = 0;
    uint64_t unixLast = 0, unixLastSector = 0, unixThis, unixFirst = 0;
    SpiFlashOpResult opResult;
    bool first = true, seenEmptyPage;

    Serial.println("Recovering on-flash records...");
    for (
      sectorID = 0,
      flashSectorID = FlashSectStart;

      sectorID < FlashSectTotal;

      ++sectorID,
      ++flashSectorID
    ) {
      for (
        sectorPageID = 0,
        flashPageID = flashSectorID << FlashSectPageFactor,
        seenEmptyPage = false;

        sectorPageID < FlashSectPageCount;

        ++sectorPageID,
        ++flashPageID
      ) {
        opResult = spi_flash_read(flashPageID << FlashPageExp, sliceL2Raw, FlashPageSize);
        if (opResult != SPI_FLASH_RESULT_OK) {
          Serial.printf("Failed to read sector %" PRIu16 "/f%" PRIu16 " page %" PRIu16 "/f%" PRIu16 " when recovering, using good pages before it\n", sectorID, flashSectorID, sectorPageID, flashPageID);
          headL2 = 0;
          countL2 = firstPageCount;
          return;
        }
        if (erasedL2()) { /* Empty page */
          if (secondSectHead > 0) {
            Serial.printf("Page %" PRIu16 " in sector %" PRIu16 "/f%" PRIu16 " is empty, when we expect second head; use only the first half of ring\n", sectorPageID, sectorID, flashSectorID);
            headL2 = 0;
            countL2 = firstPageCount;
            return;
          }
          seenEmptyPage = true;
          continue;
        }
        if (seenEmptyPage || !sliceL2.valid()) {
          if (seenEmptyPage) {
            Serial.println("Non-empty page after empty page in same sector");
          }
          Serial.printf("Page %" PRIu16 " in sector %" PRIu16 "/f%" PRIu16 " is bad; ", sectorPageID, sectorID, flashSectorID);
          if (secondSectHead > 0) {
            Serial.printf("and we were expecting second half of ring after either alike sector or empty sector, using sectors before %" PRIu16 "\n", secondSectHead);
            headL2 = 0;
            countL2 = firstPageCount;
            return;
          } else {
            Serial.println("consider this sector bad and expect next sector to be head of second part of ring");
            firstPageCount = sectorID << FlashSectPageFactor;
            secondSectHead = sectorID + 1;
            unixLast = unixLastSector;
            break;
          }
        }
        unixThis = sliceL2.unixOffset;
        if (first) { /* avoid bool(uint64_t) which needs software emulation */
          unixFirst = unixThis;
          first = false;
        }
        if (unixThis <= unixLast) { /* Jumping back, allowed only once, and only for the first page in sector */
          if (secondSectHead > 0 && (sectorID != secondSectHead || sectorPageID > 0)) {
            /* After a partial/bad sector, a jump back is only valid at the expected second head. */
            Serial.printf("Flash page %" PRIu16 " in sector %" PRIu16 "/f%" PRIu16 " jump back when we already expects second half sectors head at sector %" PRIu16 ", use pages before it\n", sectorPageID, sectorID, flashSectorID, secondSectHead);
            headL2 = 0;
            countL2 = firstPageCount;
            return;
          } else if (sectorPageID > 0) { /* not the first page, not allowed */
            Serial.printf("Flash page %" PRIu16 " in sector %" PRIu16 "/f%" PRIu16 " jump back but it's not the first page, consider this sector bad, expect next sector to be head of second\n", sectorPageID, sectorID, flashSectorID);
            firstPageCount = sectorID << FlashSectPageFactor;
            secondSectHead = sectorID + 1;
            unixLast = unixLastSector;
            break;
          } else { /* jumping back at first page, first time */
            if (!secondSectHead) {
              firstPageCount = sectorID << FlashSectPageFactor;
              secondSectHead = sectorID;
            }
          }
        } else if (!secondSectHead) {
          ++firstPageCount;
        }
        ++pageCount;
        unixLast = unixThis;
      }
      /* The above logic should guarantee when seenEmptyPage == true, secondSecHead == false, but do a check anyway */
      if (seenEmptyPage && !secondSectHead) {
        secondSectHead = sectorID + 1;
      }
      if (sectorPageID == FlashSectPageCount) {
        unixLastSector = unixLast;
      }
    }
    if (secondSectHead > 0 && unixFirst <= unixLast) {
      Serial.printf("Flash jumped back at sector %" PRIu16 " yet the first unix offset %" PRIu64 " is not larger than last offset %" PRIu64 ", use pages before jumping back\n", secondSectHead, unixFirst, unixLast);
      headL2 = 0;
      countL2 = firstPageCount;
    } else {
      headL2 = secondSectHead;
      countL2 = pageCount;
    }
  }

  void fetchAppend(uint32_t const millisCurrent) {
    struct SensorValue * value;
    uint16_t current;
    uint64_t unixSeconds;

    if (!dht.read(true)) {
      return;
    }

    if (countL0 == MaxHistoryL0) {
      if (!headL0) {
        if (countL1 == SensorSlice::MaxRecords) {
          flushL1L2();
        }
        appendL1(valuesL0[0], unixSecondsL0[0]);
      }
      current = headL0;
      headL0 = (headL0 + 1) & RingMaskL0;
    } else {
      current = (headL0 + countL0++) & RingMaskL0;
    }

    value = valuesL0 + current;

    millisLast = millisCurrent;
    unixSeconds = ntpSyncer.currentUnixSeconds(millisCurrent);
    unixSecondsL0[current] = unixSeconds;
    value->humidInt = dht.data[0];
    value->humidDot = dht.data[1];
    value->tempInt =  static_cast<int8_t>(dht.data[2]);
    value->tempDot = dht.data[3];
  }

  void firstFetchAppend(uint32_t const millisCurrent) {
    if (countL2 > 0 && fetchPage(slicePage(countL2 - 1))) {
      ntpSyncer.ensureUnixSeconds(millisCurrent, sliceL2.unixOffset + sliceL2.records[SensorSlice::MaxRecordsSub1].timestamp);
    }
    dht.begin();
    fetchAppend(millisCurrent);
  }

  void maybeFetchAppend(uint32_t const millisCurrent) {
    if (millisCurrent - millisLast >= MinInterval) {
      fetchAppend(millisCurrent);
    }
  }
};

static SensorHistory history = {};

void httpHandleLast() {
  int i, r, offset, remain;
  int const countArgs = server.args();
  SensorValue &value = history.last();
  char buffer[SensorValue::lenBufferShort];

  if (countArgs > 0) {
    offset = 0, remain = SensorValue::lenBufferShort;
    buffer[1] = '\0';
    for (i = 0; i < countArgs; ++i) {
      String const &argName = server.argName(i);
      if (argName.startsWith("temp")) {
        r = snprintf(buffer + offset, remain, ",%" PRId8 ".%" PRIu8, value.tempInt, value.tempDot);
      } else if (argName.startsWith("humid")) {
        r = snprintf(buffer + offset, remain, ",%" PRIu8 ".%" PRIu8, value.humidInt, value.humidDot);
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
    r = snprintf(buffer, SensorValue::lenBufferShort, "%" PRId8 ".%" PRIu8 ",%" PRIu8 ".%" PRIu8 "", value.tempInt, value.tempDot, value.humidInt, value.humidDot);
    if (r < 0) {
      serverSendError(StringFormatFailure);
      return;
    }
    if (r >= SensorValue::lenBufferShort) {
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
  COMPCONST size_t const lenMax = lenBuffer - SensorValue::lenBuffer;

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

  void sendValue(uint64_t const unixSeconds, SensorValue const &value) {
    if (value.intoStr(unixSeconds, buffer + offset, len)) {
      if ((offset += len) >= lenMax) {
        server.sendContent(buffer, offset);
        offset = 0;
      }
    }
  }

  void sendRingRecords(SensorValue const *values, uint64_t const *unixSeconds, uint8_t const head, uint8_t const count) {
    uint8_t i;

    for (i = head; i < count; ++i) {
      sendValue(unixSeconds[i], values[i]);
    }
    for (i = 0; i < head; ++i) {
      sendValue(unixSeconds[i], values[i]);
    }
  }

  void sendSlice(SensorSlice const &slice, uint16_t const count = SensorSlice::MaxRecords) {
    uint64_t const unixOffset = slice.unixOffset;
    SensorRecord const *record;
    uint16_t i;

    for (i = 0; i < count; ++i) {
      record = slice.records + i;
      sendValue(unixOffset + record->timestamp, record->value);
    }
  }

  void sendPage(uint16_t const pageID) {
    if (!history.fetchPage(pageID)) {
      return;
    }
    sendSlice(history.sliceL2);
  }

  void sendAll() {
    uint16_t i;

    if (history.countL2 > 0) {
      offset = history.firstPage();
      if (offset > 0) {
        for (i = offset; i < SensorHistory::FlashPageTotal; ++i) {
          sendPage(i);
        }
        offset = history.countL2 + offset - SensorHistory::FlashPageTotal;
        for (i = 0; i < offset; ++i) {
          sendPage(i);
        }
      } else {
        for (i = 0; i < history.countL2; ++i) {
          sendPage(i);
        }
      }
    }
    if (history.countL1 > 0) {
      sendSlice(history.sliceL1, history.countL1);
    }
    if (history.countL0 > 0) {
      sendRingRecords(history.valuesL0, history.unixSecondsL0, history.headL0, history.countL0);
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
  RawAllSender sender = {};

  sender.sendAll();
}

void httpHandleRawLast() {
  char buffer[SensorValue::lenBuffer];
  uint64_t unixSeconds;
  size_t len;

  if (history.last(unixSeconds).intoStr(unixSeconds, buffer, len)) {
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
      Serial.printf("Recovered %" PRIu16 " slices from flash, head is %" PRIu16 "\n", history.countL2, history.headL2);
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

  millisCurrent = upTimer.currentMillis();
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
  millisCurrent = upTimer.currentMillis();
  ntpSyncer.maybeUpdate(millisCurrent);
  history.maybeFetchAppend(millisCurrent);
}
