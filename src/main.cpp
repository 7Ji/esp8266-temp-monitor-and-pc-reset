#include <Arduino.h>
#include <DHT.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

static_assert(sizeof(unsigned long) == sizeof(uint32_t), "unsigned long (would return by millis) is not 32 bit");

#include "root.html.h"

static inline constexpr char ConfigHostName[] = PRIVATE_HOSTNAME;
static inline constexpr char ConfigWiFiSSID[] = PRIVATE_WIFI_SSID;
static inline constexpr char ConfigWiFiPassword[] = PRIVATE_WIFI_PASSWORD;
static inline constexpr unsigned long ConfigBaudRate = 115200;

static inline constexpr char ErrorStringWouldTruncate[] PROGMEM = "String Would Truncate";
static inline constexpr char ErrorStringFormatFailure[] PROGMEM = "String Format Failure";
static inline constexpr char ErrorNotFound[] PROGMEM = "Not Found";
static inline constexpr char PcPowerOk[] PROGMEM = "Power button pulse sent";
static inline constexpr char PcResetOk[] PROGMEM = "Reset button pulse sent";

static inline constexpr unsigned long OneSecondAsMs = 1'000;
static inline constexpr uint8_t MaxWaits = 20;
static inline constexpr uint8_t PcPinPower =
#ifdef PRIVATE_PIN_POWER
PRIVATE_PIN_POWER
#else
D1
#endif
;
static inline constexpr uint8_t PcPinReset =
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
#include "snippet/sharedBuffer.h"

#define serverSendError(x) server.send_P(500, "text/plain", Error ## x, sizeof(Error ## x) - 1)

struct UpTimer {
  static inline constexpr uint32_t MillisSafeMax = UINT32_MAX - OneSecondAsMs;

  uint32_t secondsOffset = 0;
  uint32_t millisOffset = 0;
  uint32_t millisLast = 0;

  [[gnu::always_inline]] inline uint32_t currentMillis() {
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

struct WiFiKeeper {
  static inline constexpr size_t WantedLen = sizeof(ConfigWiFiSSID) - 1;
  static inline constexpr uint8_t BssidSize = 6;
  /*
     0 ... 15 seconds : no reconnect
    15 ...    seconds : reconnect with current setup
    60 ...    seconds : reconnect with current setup, then rescan
  */
  static inline constexpr uint32_t ThresholdDisconnect = 15000;
  static inline constexpr uint32_t ThresholdReconnect = ThresholdDisconnect;
  static inline constexpr uint32_t ThresholdScan = 60000;

  uint8_t bssid[BssidSize] = {};
  uint8_t bssidLength = 0;
  uint8_t channel = 0;
  bool connected = false;
  uint32_t millisDisconnect = 0;
  uint32_t millisConnect = 0;
  uint32_t millisScan = 0;

  void beginChosen(uint32_t const millisCurrent) {
    Serial.printf("Connecting to ch %" PRIu8 " @ " MACSTR "\n", channel, MAC2STR(bssid));
    WiFi.begin(ConfigWiFiSSID, ConfigWiFiPassword, channel, bssidLength == BssidSize ? bssid : nullptr);
    millisConnect = millisCurrent;
  }

  bool scanChoose(uint32_t const millisCurrent) {
    bss_info const *info;
    int8_t countNetworks, i;
    int32_t bestRSSI = INT32_MIN;

    Serial.println("Scanning network...");
    countNetworks = WiFi.scanNetworks();
    if (countNetworks <= 0) {
      Serial.println("No network discovered");
      return false;
    }
    Serial.printf("Discovered %" PRId8 " networks, filtering\n", countNetworks);
    for (i = 0; i < countNetworks; ++i) {
      info = WiFi.getScanInfoByIndex(i);
      if (!info) {
        continue;
      }
      Serial.printf(
        "[%02" PRId8 "] %-20.*s (" MACSTR ") %5" PRId8 " dBm @ ch %" PRIu8 "\n",
        i,
        int(info->ssid_len), reinterpret_cast<char const *>(info->ssid),
        MAC2STR(info->bssid),
        info->rssi, info->channel
      );
      if (info->ssid_len != WantedLen || memcmp(info->ssid, ConfigWiFiSSID, WantedLen)) {
        continue;
      }
      if (info->rssi > bestRSSI) {
        if (bestRSSI == INT32_MIN) {
          Serial.println(" => FOUND");
        } else {
          Serial.println(" => REPLACE");
        }
        bestRSSI = info->rssi;
        channel = info->channel;
        bssidLength = BssidSize;
        memcpy(bssid, info->bssid, BssidSize);
      } else {
        Serial.println(" => BYPASS");
      }
    }
    WiFi.scanDelete();
    if (bestRSSI == INT32_MIN || !bssidLength) {
      Serial.println("Did not find any matching network");
      return false;
    }
    Serial.printf("Chose target AP on ch %" PRIu8 " @ " MACSTR "\n", channel, MAC2STR(bssid));
    millisScan = millisCurrent;
    return true;
  }

  void markConnected() {
    if (!connected) {
      Serial.print("WiFi IP: ");
      Serial.print(WiFi.localIP());
      Serial.print(" / ");
      Serial.print(WiFi.subnetMask());
      Serial.print(", Gateway: ");
      Serial.println(WiFi.gatewayIP());
    }
    connected = true;
    millisDisconnect = 0;
  }

  void init(uint32_t const millisCurrent) {
    for (;;) {
      if (scanChoose(millisCurrent)) {
        beginChosen(millisCurrent);
        for (uint8_t i = 0; i < MaxWaits; ++i) {
          if (WiFi.status() == WL_CONNECTED) {
            markConnected();
            return;
          }
          delay(500);
        }
      }
      Serial.println("Failed to connect, restart scanning process after 1 second");
      delay(OneSecondAsMs);
    }
  }

  [[gnu::always_inline]] inline void maybeReconnect(uint32_t const millisCurrent) {
    if (WiFi.status() == WL_CONNECTED) {
      markConnected();
      return;
    }
    if (connected) {
      Serial.println("WiFi disconnected");
      connected = false;
      millisDisconnect = millisCurrent;
    }
    if (millisCurrent - millisDisconnect < ThresholdDisconnect) {
      return;
    }
    if (millisCurrent - millisConnect >= ThresholdReconnect) {
      Serial.println("WiFi still disconnected, retrying saved AP");
      WiFi.reconnect();
      millisConnect = millisCurrent;
      delay(500);
      if (WiFi.status() == WL_CONNECTED) {
        return;
      }
    }
    if (millisCurrent - millisScan >= ThresholdScan) {
      Serial.println("WiFi still disconnected, rescanning target AP");
      if (scanChoose(millisCurrent)) {
        beginChosen(millisCurrent);
        return;
      }
    }
  }
};

static WiFiKeeper wifiKeeper = {};

struct NtpSyncer {
  static inline constexpr uint32_t MinInterval = 3600000UL;
  static inline constexpr uint32_t NtpUnixOffset = 2208988800UL;
  static inline constexpr uint32_t MinUnixOffset = 1776133834UL; /* Tue Apr 14 11:00:01 CST 2026, no specific reason */
  static inline constexpr uint32_t MinNtp = NtpUnixOffset + MinUnixOffset;
  static inline constexpr size_t BufferSize = 48;
  static inline constexpr uint16_t NtpPortRemote = 123;
  static inline constexpr uint8_t OffsetSecs = 40;
  static inline constexpr uint8_t OffsetFrac = 44;
  static inline constexpr uint8_t NtpMagic = 0b11100011;

  static inline constexpr char NtpServer[] =
#ifdef PRIVATE_NTP_SERVER
    PRIVATE_NTP_SERVER
#else
    "pool.ntp.org"
#endif
      ;
  static inline constexpr uint16_t NtpPortLocal =
#ifdef PRIVATE_NTP_LOCAL_PORT
    PRIVATE_NTP_LOCAL_PORT
#else
    7931 /* This is chosen simply randomly */
#endif
      ;

  IPAddress serverIP;
  WiFiUDP udp;
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
      discarded = udp.read(sharedBytesBuffer, size > BufferSize ? BufferSize : size);
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
    sharedBytesBuffer[0] = NtpMagic;
    memset(sharedBytesBuffer + 1, 0, BufferSize - 1);
    requestSecs = ++requestToken;
    requestFrac = millisCurrent;
    BEu32To(sharedBytesBuffer + OffsetSecs, requestSecs);
    BEu32To(sharedBytesBuffer + OffsetFrac, requestFrac);
    if (udp.write(sharedBytesBuffer, BufferSize) != BufferSize) {
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
        if (udp.read(sharedBytesBuffer, BufferSize) != BufferSize) {
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
        if (BEu32At(sharedBytesBuffer + 24) != requestSecs || BEu32At(sharedBytesBuffer + 28) != requestFrac) {
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
    ntpOffset = BEu32At(sharedBytesBuffer + OffsetSecs);

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

  [[gnu::always_inline]] inline void maybeUpdate(uint32_t const millisCurrent) {
    /* Force update if not initialized yet, or */
    if ((millisCurrent - millisLast) >= MinInterval || !init) {
      update(millisCurrent);
    }
  }
};

static NtpSyncer ntpSyncer = {};
static_assert(SharedBufferSize >= NtpSyncer::BufferSize, "Shared buffer is smaller than NTP buffer");

#include "snippet/sensorPage.h"
bool SensorValue::intoStr(uint64_t const unixSeconds, char *const buffer, size_t &len) const {
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
uint32_t SensorPage::actualChecksum() const {
  return crc32(&unixOffset, sizeof(unixOffset) + sizeof(records));
}
#include "snippet/recoverFlash.h"
[[gnu::always_inline]] inline bool RecoverFlash::readFlashSector(uint16_t const flashSectorID) {
  SpiFlashOpResult const opResult = spi_flash_read(flashSectorID << FlashStats::SectExp, sharedWordsBuffer, FlashStats::SectSize);
  if (opResult != SPI_FLASH_RESULT_OK) {
    PRINTF("Failed to read flash sector f%" PRIu16 " when recovering flash, opResult %d\n", flashSectorID, opResult);
    return false;
  }
  return true;
}

struct SensorHistory {
  static inline constexpr uint32_t MinInterval = 2000;
  static inline constexpr uint8_t MaxHistoryL0 = 1 << 5; /* 2s * 32, even secondly, for about a minute */

  static inline constexpr uint8_t RingMaskL0 = MaxHistoryL0 - 1;

  static_assert(sizeof(struct SensorPage) == FlashStats::PageSize, "SensorPage should have the same size as page");

  union {
    SensorPage pageL1 = {.magic = SensorPage::Magic};
    uint32_t pageL1Raw[sizeof(SensorPage) / sizeof(uint32_t)];
  };
  union {
    SensorPage pagesL2x16[FlashStats::SectPageCount];
    uint32_t pagesL2x16Raw[FlashStats::SectWordCount];
  };
  SensorValue valuesL0[MaxHistoryL0];
  uint64_t unixSecondsL0[MaxHistoryL0];
  uint32_t millisLast = 0;
  uint16_t countL2 = 0; /* page */
  uint16_t headL2 = 0; /* sector */
  uint16_t countL1 = 0;
  uint16_t sectorCachedL2 = FlashStats::SectTotal;
  uint8_t headL0 = 0;
  uint8_t countL0 = 0;

  void invalidateCachedL2(uint16_t const sectorID) {
    if (sectorCachedL2 == sectorID) {
      sectorCachedL2 = FlashStats::SectTotal;
    }
  }

  uint16_t firstPage() const {
    return headL2 << FlashStats::SectPageFactor;
  }

  uint16_t historyPage(uint16_t const pageID) const {
    return (firstPage() + pageID) % FlashStats::PageTotal;
  }

  uint32_t totalCount() const {
    return uint32_t(countL2) * SensorPage::MaxRecords + countL1 + countL0;
  }

  SensorValue &first() {
    SensorPage *page;

    if (countL2 > 0 && (page = fetchPageL2(firstPage())) != nullptr) {
      return page->records[0].value;
    } else if (countL1 > 0) {
      return pageL1.records[0].value;
    } else {
      return valuesL0[headL0];
    }
  }

  SensorValue &first(uint64_t &unixSeconds) {
    SensorPage *page;

    if (countL2 > 0 && (page = fetchPageL2(firstPage())) != nullptr) {
      unixSeconds = page->unixOffset + page->records[0].timestamp;
      return page->records[0].value;
    } else if (countL1 > 0) {
      unixSeconds = pageL1.unixOffset + pageL1.records[0].timestamp;
      return pageL1.records[0].value;
    } else {
      unixSeconds = unixSecondsL0[headL0];
      return valuesL0[headL0];
    }
  }

  uint64_t firstUnixSeconds() {
    SensorPage *page;

    if (countL2 > 0 && (page = fetchPageL2(firstPage())) != nullptr) {
      return page->unixOffset + page->records[0].timestamp;
    } else if (countL1 > 0) {
      return pageL1.unixOffset + pageL1.records[0].timestamp;
    } else {
      return unixSecondsL0[headL0];
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
    uint16_t const flashSectorID = sectorID + FlashStats::SectStart;
    SpiFlashOpResult opResult;

    invalidateCachedL2(sectorID);
    Serial.printf("Erasing sector %" PRIu16 "/f%" PRIu16 "\n", sectorID, flashSectorID);
    opResult = spi_flash_erase_sector(flashSectorID);
    if (opResult != SPI_FLASH_RESULT_OK) {
      Serial.printf("Failed to erase sector %" PRIu16 "/f%" PRIu16 ", op result %d\n", sectorID, flashSectorID, opResult);
      return false;
    }
    return true;
  }

  bool writeL1(uint16_t const pageID) {
    uint16_t const flashPageID = pageID + FlashStats::PageStart;
    SpiFlashOpResult opResult;

    invalidateCachedL2(pageID >> FlashStats::SectPageFactor);
    Serial.printf("Writing L1 to page %" PRIu16 "/f%" PRIu16 "\n", pageID, flashPageID);
    opResult = spi_flash_write(flashPageID * FlashStats::PageSize, pageL1Raw, FlashStats::PageSize);
    if (opResult != SPI_FLASH_RESULT_OK) {
      Serial.printf("Failed to write L1 to page %" PRIu16 "/f%" PRIu16 ", op result %d\n", pageID, flashPageID, opResult);
      return false;
    }
    return true;
  }

  void fallbackShift() {
    Serial.println("Falling back to shift L1 by one position");
    pageL1.shift();
    countL1 = SensorPage::MaxRecordsSub1;
  }

  void appendL1(SensorValue const &valueL0, uint64_t const unixSeconds) {
    uint64_t diff64;
    SensorRecord recordL1 = {.value = valueL0};

    if (countL1 > 0) {
      /* Unlikely to happen, but just in case */
      diff64 = unixSeconds - pageL1.unixOffset;
      if (diff64 > UINT16_MAX) {
        Serial.printf("Dropping partial L1 with too-wide timestamp delta %" PRIu64 "\n", diff64);
        countL1 = 0;
      }
    }

    if (countL1 == 0) {
      pageL1.unixOffset = unixSeconds;
      recordL1.timestamp = 0;
    } else {
      recordL1.timestamp = unixSeconds - pageL1.unixOffset;
    }
    pageL1.records[countL1++] = recordL1;
  }

  void flushL1L2() {
    uint16_t const pageID = historyPage(countL2);
    bool const afterBoundary = pageID & FlashStats::PageInSectMask;

    Serial.printf("Flushing L1 to L2 flash page %" PRIu16 "\n", pageID);

    if (!afterBoundary) {
      /* Whether we successfully erase the sector or not, consider pages on it bad */
      if (countL2 > FlashStats::PageTotalSubSect) { /* Technically this could only be countL2 == FlashStats::PageTotal, but do this securely */
        headL2 = (headL2 + 1) % FlashStats::SectTotal;
        countL2 = FlashStats::PageTotalSubSect;
      }
      if (!erase(pageID >> FlashStats::SectPageFactor)) {
        fallbackShift();
        return;
      }
    }
    pageL1.checksum = pageL1.actualChecksum();
    if (!writeL1(pageID)) {
      fallbackShift();
      return;
    }
    countL1 = 0;
    ++countL2;
  }

  bool fetchSector(uint16_t const sectorID) {
    SpiFlashOpResult opResult;
    uint16_t const flashSectorID = sectorID + FlashStats::SectStart;

    if (sectorCachedL2 == sectorID) {
      return true;
    }

    opResult = spi_flash_read(flashSectorID << FlashStats::SectExp, pagesL2x16Raw, FlashStats::SectSize);
    if (opResult != SPI_FLASH_RESULT_OK) {
      Serial.printf("fetch sector %" PRIu16 "/f%" PRIu16 ": Read not OK, op result %d\n", sectorID, flashSectorID, opResult);
      return false;
    }
    sectorCachedL2 = sectorID;
    return true;
  }

  SensorPage *fetchPageL2(uint16_t const pageID) {
    SensorPage *page;

    if (!fetchSector(pageID >> FlashStats::SectPageFactor)) {
      return nullptr;
    }
    page = pagesL2x16 + (pageID & FlashStats::PageInSectMask);
    return page->valid() ? page : nullptr;
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
        if (countL1 == SensorPage::MaxRecords) {
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
    SensorPage *page;

    if (countL2 > 0 && (page = fetchPageL2(historyPage(countL2 - 1))) != nullptr) {
      ntpSyncer.ensureUnixSeconds(millisCurrent, page->unixOffset + page->records[SensorPage::MaxRecordsSub1].timestamp);
    }
    dht.begin();
    fetchAppend(millisCurrent);
  }

  [[gnu::always_inline]] inline void maybeFetchAppend(uint32_t const millisCurrent) {
    if (millisCurrent - millisLast >= MinInterval) {
      fetchAppend(millisCurrent);
    }
  }
};

static SensorHistory history = {};

void httpHandleLast() {
  SensorValue &value = history.last();
  int r;

  r = snprintf(sharedStrBuffer, SensorValue::lenBufferTempAndHumid, "%" PRId8 ".%" PRIu8 ",%" PRIu8 ".%" PRIu8 "", value.tempInt, value.tempDot, value.humidInt, value.humidDot);
  if (r < 0) {
    serverSendError(StringFormatFailure);
    return;
  }
  if (r >= SensorValue::lenBufferTempAndHumid) {
    serverSendError(StringWouldTruncate);
    return;
  }
  server.send(200, "text/plain", sharedStrBuffer, r);
}

void httpHandleTemp() {
  SensorValue &value = history.last();
  int r;

  r = snprintf(sharedStrBuffer, SensorValue::lenBufferTempOrHumid, "%" PRId8 ".%" PRIu8 "", value.tempInt, value.tempDot);
  if (r < 0) {
    serverSendError(StringFormatFailure);
    return;
  }
  if (r >= SensorValue::lenBufferTempOrHumid) {
    serverSendError(StringWouldTruncate);
    return;
  }
  server.send(200, "text/plain", sharedStrBuffer, r);
}

void httpHandleHumid() {
  SensorValue &value = history.last();
  int r;

  r = snprintf(sharedStrBuffer, SensorValue::lenBufferTempOrHumid, "%" PRIu8 ".%" PRIu8 "", value.humidInt, value.humidDot);
  if (r < 0) {
    serverSendError(StringFormatFailure);
    return;
  }
  if (r >= SensorValue::lenBufferTempOrHumid) {
    serverSendError(StringWouldTruncate);
    return;
  }
  server.send(200, "text/plain", sharedStrBuffer, r);
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

void httpHandlePower() {
  if (!serverIsAuthorized()) {
    httpHandleNotFound();
    return;
  }

  pulseButton(PcPinPower);
  server.send_P(200, "text/plain", PcPowerOk, sizeof(PcPowerOk) - 1);
}

void httpHandleReset() {
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

template<typename Each>
struct AllSender {
  Each each;

  void sendRingRecords() {
    uint8_t i;

    for (i = history.headL0; i < history.countL0; ++i) {
      each.sendValue(history.unixSecondsL0[i], history.valuesL0[i]);
    }
    for (i = 0; i < history.headL0; ++i) {
      each.sendValue(history.unixSecondsL0[i], history.valuesL0[i]);
    }
  }

  void sendPage(SensorPage const &page, uint16_t const count = SensorPage::MaxRecords) {
    uint64_t const pageUnixOffset = page.unixOffset;
    SensorRecord const *record;
    uint16_t i;

    for (i = 0; i < count; ++i) {
      record = page.records + i;
      each.sendValue(pageUnixOffset + record->timestamp, record->value);
    }
  }

  void sendSector(uint16_t const sectorID, uint8_t const count) {
    SensorPage *page;
    uint8_t pageID;

    if (!history.fetchSector(sectorID)) {
      return;
    }
    for (pageID = 0, page = history.pagesL2x16; pageID < count; ++pageID, ++page) {
      if (page->valid()) {
        sendPage(*page);
      }
    }
  }

  void sendAll() {
    uint8_t countLast;
    uint16_t sectorCount, sectorCountFirst, sectorID, sectorEnd;

    if (history.countL2 > 0) {
      sectorCount = history.countL2 >> FlashStats::SectPageFactor;
      if (history.headL2 > 0) {
        sectorEnd = history.headL2 + sectorCount;
        if (sectorEnd > FlashStats::SectTotal) {
          sectorCountFirst = sectorEnd - FlashStats::SectTotal;
          sectorEnd = FlashStats::SectTotal;
        } else {
          sectorCountFirst = 0;
        }
        for (sectorID = history.headL2; sectorID < sectorEnd; ++sectorID) {
          sendSector(sectorID, FlashStats::SectPageCount);
        }
        if (sectorCountFirst) {
          for (sectorID = 0; sectorID < sectorCountFirst; ++sectorID) {
            sendSector(sectorID, FlashStats::SectPageCount);
          }
        }
      } else {
        for (sectorID = 0; sectorID < sectorCount; ++sectorID) {
          sendSector(sectorID, FlashStats::SectPageCount);
        }
      }
      countLast = history.countL2 & FlashStats::PageInSectMask;
      if (countLast > 0) {
        sendSector(sectorID, countLast);
      }
    }
    if (history.countL1 > 0) {
      sendPage(history.pageL1, history.countL1);
    }
    if (history.countL0 > 0) {
      sendRingRecords();
    }
  }

  ~AllSender() {
    if (each.offset) {
      server.sendContent(sharedStrBuffer, each.offset);
      each.offset = 0;
    }
    server.sendContent("", 0);
  }
};

struct HistoryEach {
  static inline constexpr size_t lenMax = SharedBufferSize - SensorValue::lenBuffer;

  size_t len;
  size_t offset = 0;

  HistoryEach() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
#ifdef PRIVATE_ALLOW_CROSS
    serverSendHeadersForRaw();
#endif
    server.send(200, "text/plain", "", 0);
  }

  void sendValue(uint64_t const unixSeconds, SensorValue const &value) {
    if (value.intoStr(unixSeconds, sharedStrBuffer + offset, len)) {
      if ((offset += len) >= lenMax) {
        server.sendContent(sharedStrBuffer, offset);
        offset = 0;
      }
    }
  }
};

void httpHandleHistory() {
  AllSender<HistoryEach> sender = {};

  sender.sendAll();
}

struct DumpRecord {
  uint32_t timestamp;
  SensorValue value;
};

static_assert(sizeof(struct DumpRecord) == 8, "DumpRecord should have a size of 8");

struct DumpEach {
  static inline constexpr size_t lenMax = SharedBufferSize - sizeof(DumpRecord);
  static inline constexpr uint32_t Magic = 0x01323845; /* E82\1 */

  uint64_t unixOffset = history.firstUnixSeconds();
  size_t offset = 0;

  DumpEach() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
#ifdef PRIVATE_ALLOW_CROSS
    serverSendHeadersForRaw();
#endif
    *sharedWordsBuffer = Magic;
    memcpy(sharedWordsBuffer + 1, &unixOffset, sizeof(unixOffset));
    server.send(200, "application/octet-stream", sharedBytesBuffer, sizeof(Magic) + sizeof(unixOffset));
  }

  void sendValue(uint64_t const unixSeconds, SensorValue const &value) {
    DumpRecord *const record = reinterpret_cast<DumpRecord *>(sharedBytesBuffer + offset);

    record->timestamp = unixSeconds - unixOffset;
    record->value = value;
    if ((offset += sizeof(DumpRecord)) >= lenMax) {
      server.sendContent(sharedStrBuffer, offset);
      offset = 0;
    }
  }
};

void httpHandleDump() {
  AllSender<DumpEach> sender = {};

  sender.sendAll();
}

void httpHandleRecent() {
  uint64_t unixSeconds;
  size_t len;

  if (history.last(unixSeconds).intoStr(unixSeconds, sharedStrBuffer, len)) {
#ifdef PRIVATE_ALLOW_CROSS
    serverSendHeadersForRaw();
#endif
    server.send(200, "text/plain", sharedStrBuffer, len);
  }
}

void httpHandleRoot() {
  server.send_P(200, "text/html; charset=utf-8", RootPage, sizeof(RootPage) - 1);
}

void setup() {
  uint32_t millisCurrent;

  pinMode(PcPinPower, OUTPUT);
  digitalWrite(PcPinPower, LOW);
  pinMode(PcPinReset, OUTPUT);
  digitalWrite(PcPinReset, LOW);

  Serial.begin(ConfigBaudRate);
  RecoverFlash::recoverFlash(history.headL2, history.countL2);

  WiFi.setHostname(ConfigHostName);
  WiFi.setAutoReconnect(true);

  millisCurrent = upTimer.currentMillis();
  wifiKeeper.init(millisCurrent);
  ntpSyncer.firstUpdate(millisCurrent);
  history.firstFetchAppend(millisCurrent);

  server.on("/", HTTP_GET, httpHandleRoot);
  server.on("/last", HTTP_GET, httpHandleLast);
  server.on("/temp", HTTP_GET, httpHandleTemp);
  server.on("/humid", HTTP_GET, httpHandleHumid);
  server.on("/power", HTTP_POST, httpHandlePower);
  server.on("/reset", HTTP_POST, httpHandleReset);
  server.on("/recent", HTTP_GET, httpHandleRecent);
  server.on("/history", HTTP_GET, httpHandleHistory);
  server.on("/dump", HTTP_GET, httpHandleDump);
  server.onNotFound(httpHandleNotFound);
  server.begin();

  Serial.println("HTTP Server started");
}

void loop() {
  uint32_t millisCurrent;

  server.handleClient();
  millisCurrent = upTimer.currentMillis();
  wifiKeeper.maybeReconnect(millisCurrent);
  ntpSyncer.maybeUpdate(millisCurrent);
  history.maybeFetchAppend(millisCurrent);
}
