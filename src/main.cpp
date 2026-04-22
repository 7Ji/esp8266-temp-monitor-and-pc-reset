#include <Arduino.h>
#include <DHT.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

static_assert(sizeof(unsigned long) == sizeof(uint32_t), "unsigned long (would return by millis) is not 32 bit");

#include "root.html.h"
#include "snippet/compConst.h"

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
#include "snippet/sharedBuffer.h"

#define serverSendError(x) server.send_P(500, "text/plain", Error ## x, sizeof(Error ## x) - 1)

struct UpTimer {
  COMPCONST uint32_t const MillisSafeMax = UINT32_MAX - OneSecondAsMs;

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
  COMPCONST size_t const WantedLen = sizeof(ConfigWiFiSSID) - 1;
  COMPCONST uint8_t const BssidSize = 6;
  /*
     0 ... 15 seconds : no reconnect
    15 ...    seconds : reconnect with current setup
    60 ...    seconds : reconnect with current setup, then rescan
  */
  COMPCONST uint32_t const ThresholdDisconnect = 15000;
  COMPCONST uint32_t const ThresholdReconnect = ThresholdDisconnect;
  COMPCONST uint32_t const ThresholdScan = 60000;

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

struct SensorValue {
  COMPCONST int const lenBufferTempOrHumid = 8; /* extreme 255.255\0 */
  COMPCONST int const lenBufferTempAndHumid = lenBufferTempOrHumid * 2; /* extreme 255.255,255.255\0 */
  COMPCONST int const lenBuffer = lenBufferTempAndHumid + 22; /* extreme 18446744073709551615,255.255,255.255\n\0 */

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

struct DumpRecord {
  uint32_t timestamp;
  SensorValue value;
};

static_assert(sizeof(struct DumpRecord) == 8, "DumpRecord should have a size of 8");

COMPCONST size_t const DumpHeaderSize = 16;

struct ShadowEntry {
  uint16_t timestamp; /* second offset from slice unixOffset */
  SensorValue value;
  uint8_t index;
  uint8_t reserved;
  uint32_t checksum;

  uint32_t actualChecksum() const {
    return crc32(this, sizeof(timestamp) + sizeof(value) + sizeof(index) + sizeof(reserved));
  }

  void finalize(uint16_t const timestampNext, SensorValue const &valueNext, uint8_t const indexNext) {
    timestamp = timestampNext;
    value = valueNext;
    index = indexNext;
    reserved = UINT8_MAX;
    checksum = actualChecksum();
  }

  bool valid(uint8_t const expectedIndex) const {
    return index == expectedIndex && reserved == UINT8_MAX && checksum == actualChecksum();
  }
};

static_assert(sizeof(struct ShadowEntry) == 12, "ShadowEntry should have a size of 12");

struct ShadowPage0 {
  COMPCONST uint32_t const Magic = 0x30444853; /* SHD0 */
  COMPCONST uint8_t const EntryCount = 20;

  uint64_t unixOffset;
  uint32_t magic;
  uint32_t checksum;
  ShadowEntry entries[EntryCount];

  uint32_t actualChecksum() const {
    return crc32(this, sizeof(unixOffset) + sizeof(magic));
  }

  void initialize(uint64_t const unixOffsetNext = 0) {
    memset(this, UINT8_MAX, sizeof(*this));
    unixOffset = unixOffsetNext;
    magic = Magic;
    checksum = actualChecksum();
  }

  bool validHeader() const {
    return magic == Magic && checksum == actualChecksum();
  }
};

static_assert(sizeof(struct ShadowPage0) == 256, "ShadowPage0 should have the same size as page");

struct ShadowPage1 {
  COMPCONST uint32_t const Magic = 0x31444853; /* SHD1 */
  COMPCONST uint8_t const EntryCount = 21;

  uint32_t magic;
  ShadowEntry entries[EntryCount];

  void initialize() {
    memset(this, UINT8_MAX, sizeof(*this));
    magic = Magic;
  }

  bool validHeader() const {
    return magic == Magic;
  }
};

static_assert(sizeof(struct ShadowPage1) == 256, "ShadowPage1 should have the same size as page");

#include "snippet/sensorSlice.h"
#include "snippet/flashStats.h"
#include "snippet/recoverFlash.h"

struct SensorHistory {
  COMPCONST uint32_t const MinInterval = 2000;
  COMPCONST uint8_t const MaxHistoryL0 = 1 << 5; /* 2s * 32, even secondly, for about a minute */
  COMPCONST uint8_t const RingMaskL0 = MaxHistoryL0 - 1;
  COMPCONST uint8_t const ShadowSectorCount = 2;

  static_assert(sizeof(struct SensorSlice) == FlashStats::PageSize, "SensorSlice should have the same size as page");

  union {
    SensorSlice sliceL1;
    uint32_t sliceL1Raw[sizeof(SensorSlice) / sizeof(uint32_t)];
  };
  union {
    SensorSlice slicesL2x16[FlashStats::SectPageCount];
    uint32_t slicesL2x16Raw[FlashStats::SectWordCount];
  };
  union {
    ShadowPage0 shadowPage0;
    uint32_t shadowPage0Raw[FlashStats::PageWordCount];
  };
  union {
    ShadowPage1 shadowPage1;
    uint32_t shadowPage1Raw[FlashStats::PageWordCount];
  };
  SensorValue valuesL0[MaxHistoryL0];
  uint64_t unixSecondsL0[MaxHistoryL0];
  uint32_t millisLast = 0;
  uint16_t countL2 = 0; /* page */
  uint16_t headL2 = 0; /* sector */
  uint32_t countRecordsL2 = 0;
  uint16_t countL1 = 0;
  uint16_t sectorCachedL2 = FlashStats::SectTotal;
  uint8_t headL0 = 0;
  uint8_t countL0 = 0;

  SensorHistory() {
    sliceL1.clear();
    shadowPage0.initialize();
    shadowPage1.initialize();
  }

  void invalidateCachedL2(uint16_t const sectorID) {
    if (sectorCachedL2 == sectorID) {
      sectorCachedL2 = FlashStats::SectTotal;
    }
  }

  uint16_t dataSectorCapacity() const {
    return FlashStats::SectTotal - ShadowSectorCount;
  }

  uint16_t firstPage() const {
    return headL2 << FlashStats::SectPageFactor;
  }

  uint16_t effectivePageCapacity() const {
    return dataSectorCapacity() << FlashStats::SectPageFactor;
  }

  uint16_t slicePage(uint16_t const sliceID) const {
    return (firstPage() + sliceID) % effectivePageCapacity();
  }

  uint16_t shadowSectorBase() const {
    return dataSectorCapacity();
  }

  uint16_t shadowSectorID(uint8_t const shadowID) const {
    return shadowSectorBase() + shadowID;
  }

  uint16_t shadowPageID(uint8_t const shadowID, uint16_t const pageOffset) const {
    return (shadowSectorID(shadowID) << FlashStats::SectPageFactor) | (pageOffset & FlashStats::PageInSectMask);
  }

  uint32_t totalCount() const {
    return countRecordsL2 + countL1 + countL0;
  }

  bool readPage(uint16_t const pageID, uint32_t *const raw) {
    uint16_t const flashPageID = pageID + FlashStats::PageStart;
    SpiFlashOpResult const opResult = spi_flash_read(flashPageID * FlashStats::PageSize, raw, FlashStats::PageSize);

    if (opResult != SPI_FLASH_RESULT_OK) {
      Serial.printf("Failed to read page %" PRIu16 "/f%" PRIu16 ", op result %d\n", pageID, flashPageID, opResult);
      return false;
    }
    return true;
  }

  bool writePage(uint16_t const pageID, uint32_t *const raw, char const *const label) {
    uint16_t const flashPageID = pageID + FlashStats::PageStart;
    SpiFlashOpResult opResult;

    invalidateCachedL2(pageID >> FlashStats::SectPageFactor);
    Serial.printf("Writing %s to page %" PRIu16 "/f%" PRIu16 "\n", label, pageID, flashPageID);
    opResult = spi_flash_write(flashPageID * FlashStats::PageSize, raw, FlashStats::PageSize);
    if (opResult != SPI_FLASH_RESULT_OK) {
      Serial.printf("Failed to write %s to page %" PRIu16 "/f%" PRIu16 ", op result %d\n", label, pageID, flashPageID, opResult);
      return false;
    }
    return true;
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

  bool eraseShadowSectors() {
    uint8_t shadowID;

    for (shadowID = 0; shadowID < ShadowSectorCount; ++shadowID) {
      if (!erase(shadowSectorID(shadowID))) {
        return false;
      }
    }
    shadowPage0.initialize();
    shadowPage1.initialize();
    return true;
  }

  bool writeL1(uint16_t const pageID) {
    return writePage(pageID, sliceL1Raw, "L1");
  }

  void fallbackShift() {
    Serial.println("Falling back to shift L1 by one position");
    sliceL1.shift();
    countL1 = sliceL1.count;
  }

  uint16_t pageOffsetL2() const {
    return countL2 & FlashStats::PageInSectMask;
  }

  bool writeShadowHeader0() {
    shadowPage0.initialize(sliceL1.unixOffset);
    return writePage(shadowPageID(0, pageOffsetL2()), shadowPage0Raw, "shadow header 0");
  }

  bool writeShadowHeader1() {
    shadowPage1.initialize();
    return writePage(shadowPageID(1, pageOffsetL2()), shadowPage1Raw, "shadow header 1");
  }

  bool appendShadowEntry(uint8_t const index, SensorRecord const &record) {
    if (index < ShadowPage0::EntryCount) {
      shadowPage0.entries[index].finalize(record.timestamp, record.value, index);
      return writePage(shadowPageID(0, pageOffsetL2()), shadowPage0Raw, "shadow page 0");
    }
    if (index == ShadowPage0::EntryCount && !writeShadowHeader1()) {
      return false;
    }
    shadowPage1.entries[index - ShadowPage0::EntryCount].finalize(record.timestamp, record.value, index);
    return writePage(shadowPageID(1, pageOffsetL2()), shadowPage1Raw, "shadow page 1");
  }

  uint8_t scanShadowEntries(SensorSlice &slice) {
    uint8_t count = 0;

    if (!shadowPage0.validHeader()) {
      return 0;
    }

    slice.clear();
    slice.unixOffset = shadowPage0.unixOffset;
    while (count < ShadowPage0::EntryCount && shadowPage0.entries[count].valid(count)) {
      slice.records[count].timestamp = shadowPage0.entries[count].timestamp;
      slice.records[count].value = shadowPage0.entries[count].value;
      ++count;
    }
    if (count < ShadowPage0::EntryCount || !shadowPage1.validHeader()) {
      return count;
    }
    while (count < SensorSlice::MaxRecords && shadowPage1.entries[count - ShadowPage0::EntryCount].valid(count)) {
      slice.records[count].timestamp = shadowPage1.entries[count - ShadowPage0::EntryCount].timestamp;
      slice.records[count].value = shadowPage1.entries[count - ShadowPage0::EntryCount].value;
      ++count;
    }
    return count;
  }

  bool recoverShadow() {
    uint8_t countShadow;

    if (!readPage(shadowPageID(0, pageOffsetL2()), shadowPage0Raw)) {
      return false;
    }
    if (!readPage(shadowPageID(1, pageOffsetL2()), shadowPage1Raw)) {
      return false;
    }
    countShadow = scanShadowEntries(sliceL1);
    if (!countShadow) {
      sliceL1.clear();
      countL1 = 0;
      return true;
    }
    sliceL1.finalize(countShadow);
    countL1 = countShadow;
    return true;
  }

  void maybeSealRecoveredL1(uint64_t const unixSeconds) {
    if (countL1 > 0 && unixSeconds - sliceL1.unixOffset > UINT16_MAX) {
      flushL1L2();
    }
  }

  void appendL1(SensorValue const &valueL0, uint64_t const unixSeconds) {
    SensorRecord recordL1 = {.value = valueL0};

    if (countL1 > 0) {
      if (unixSeconds - sliceL1.unixOffset > UINT16_MAX) {
        Serial.printf("Sealing partial L1 with too-wide timestamp delta %" PRIu64 "\n", unixSeconds - sliceL1.unixOffset);
        flushL1L2();
      }
    }

    if (countL1 == 0) {
      sliceL1.clear();
      sliceL1.unixOffset = unixSeconds;
      recordL1.timestamp = 0;
      if (!writeShadowHeader0()) {
        return;
      }
    } else {
      recordL1.timestamp = unixSeconds - sliceL1.unixOffset;
    }
    if (!appendShadowEntry(countL1, recordL1)) {
      return;
    }
    sliceL1.records[countL1++] = recordL1;
    sliceL1.count = countL1;
  }

  void flushL1L2() {
    uint16_t const pageID = slicePage(countL2);
    bool const afterBoundary = pageID & FlashStats::PageInSectMask;
    SensorSlice *sliceDropped;

    if (!countL1) {
      return;
    }

    Serial.printf("Flushing L1 to L2 flash page %" PRIu16 "\n", pageID);

    if (!afterBoundary) {
      if (countL2 >= effectivePageCapacity()) {
        sliceDropped = fetchSliceL2(firstPage());
        if (sliceDropped != nullptr) {
          countRecordsL2 -= sliceDropped->count;
        }
        headL2 = (headL2 + 1) % dataSectorCapacity();
        countL2 = effectivePageCapacity() - 1;
      }
      if (!erase(pageID >> FlashStats::SectPageFactor)) {
        fallbackShift();
        return;
      }
    }

    sliceL1.finalize(countL1);
    if (!writeL1(pageID)) {
      fallbackShift();
      return;
    }
    countRecordsL2 += countL1;
    countL1 = 0;
    sliceL1.clear();
    ++countL2;
    eraseShadowSectors();
  }

  bool fetchSector(uint16_t const sectorID) {
    SpiFlashOpResult opResult;
    uint16_t const flashSectorID = sectorID + FlashStats::SectStart;

    if (sectorCachedL2 == sectorID) {
      return true;
    }

    opResult = spi_flash_read(flashSectorID << FlashStats::SectExp, slicesL2x16Raw, FlashStats::SectSize);
    if (opResult != SPI_FLASH_RESULT_OK) {
      Serial.printf("fetch sector %" PRIu16 "/f%" PRIu16 ": Read not OK, op result %d\n", sectorID, flashSectorID, opResult);
      return false;
    }
    sectorCachedL2 = sectorID;
    return true;
  }

  SensorSlice *fetchSliceL2(uint16_t const pageID) {
    SensorSlice *slice;

    if (!fetchSector(pageID >> FlashStats::SectPageFactor)) {
      return nullptr;
    }
    slice = slicesL2x16 + (pageID & FlashStats::PageInSectMask);
    return slice->valid() ? slice : nullptr;
  }

  void rebuildCountRecordsL2() {
    uint16_t pageIndex;
    SensorSlice *slice;

    countRecordsL2 = 0;
    for (pageIndex = 0; pageIndex < countL2; ++pageIndex) {
      slice = fetchSliceL2(slicePage(pageIndex));
      if (slice != nullptr) {
        countRecordsL2 += slice->count;
      }
    }
  }

  SensorValue &first() {
    SensorSlice *slice;

    if (countL2 > 0 && (slice = fetchSliceL2(firstPage())) != nullptr) {
      return slice->records[0].value;
    } else if (countL1 > 0) {
      return sliceL1.records[0].value;
    } else {
      return valuesL0[headL0];
    }
  }

  SensorValue &first(uint64_t &unixSeconds) {
    SensorSlice *slice;

    if (countL2 > 0 && (slice = fetchSliceL2(firstPage())) != nullptr) {
      unixSeconds = slice->unixOffset + slice->records[0].timestamp;
      return slice->records[0].value;
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

  void fetchAppend(uint32_t const millisCurrent) {
    struct SensorValue *value;
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
    maybeSealRecoveredL1(unixSeconds);
    unixSecondsL0[current] = unixSeconds;
    value->humidInt = dht.data[0];
    value->humidDot = dht.data[1];
    value->tempInt =  static_cast<int8_t>(dht.data[2]);
    value->tempDot = dht.data[3];
  }

  void firstFetchAppend(uint32_t const millisCurrent) {
    SensorSlice *slice;

    millisLast = millisCurrent;
    if (countL2 > 0 && (slice = fetchSliceL2(slicePage(countL2 - 1))) != nullptr) {
      ntpSyncer.ensureUnixSeconds(millisCurrent, slice->unixOffset + slice->records[slice->count - 1].timestamp);
    }
    rebuildCountRecordsL2();
    recoverShadow();
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

  void sendSlice(SensorSlice const &slice, uint16_t const count = SensorSlice::MaxRecords) {
    uint64_t const sliceUnixOffset = slice.unixOffset;
    SensorRecord const *record;
    uint16_t i;

    for (i = 0; i < count; ++i) {
      record = slice.records + i;
      each.sendValue(sliceUnixOffset + record->timestamp, record->value);
    }
  }

  void sendSector(uint16_t const sectorID, uint8_t const count) {
    SensorSlice *slice;
    uint8_t pageID;

    if (!history.fetchSector(sectorID)) {
      return;
    }
    for (pageID = 0, slice = history.slicesL2x16; pageID < count; ++pageID, ++slice) {
      if (slice->valid()) {
        sendSlice(*slice, slice->count);
      }
    }
  }

  void sendAll() {
    uint8_t countLast;
    uint16_t sectorCount, sectorCountFirst, sectorOffset, sectorID;

    if (history.countL2 > 0) {
      sectorCount = history.countL2 >> FlashStats::SectPageFactor;
      countLast = history.countL2 & FlashStats::PageInSectMask;
      sectorOffset = history.headL2;
      if (sectorOffset > 0) {
        sectorCountFirst = FlashStats::SectTotal - sectorOffset;
        if (sectorCountFirst > sectorCount) {
          sectorCountFirst = sectorCount;
        }
        for (sectorID = sectorOffset; sectorID < sectorOffset + sectorCountFirst; ++sectorID) {
          sendSector(sectorID, FlashStats::SectPageCount);
        }
        sectorCount -= sectorCountFirst;
      }
      for (sectorID = 0; sectorID < sectorCount; ++sectorID) {
        sendSector(sectorID, FlashStats::SectPageCount);
      }
      if (countLast > 0) {
        sendSector(sectorID, countLast);
      }
    }
    if (history.countL1 > 0) {
      sendSlice(history.sliceL1, history.countL1);
    }
    if (history.countL0 > 0) {
      sendRingRecords();
    }
  }

  void flushBuffered() {
    if (each.offset) {
      server.sendContent(sharedStrBuffer, each.offset);
      each.offset = 0;
    }
  }
};

struct HistoryEach {
  COMPCONST size_t const lenMax = SharedBufferSize - SensorValue::lenBuffer;

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
  sender.flushBuffered();
  server.sendContent("", 0);
}

struct DumpEach {
  COMPCONST size_t const lenMax = SharedBufferSize - sizeof(DumpRecord);

  uint64_t unixOffset = 0;
  size_t offset = 0;

  DumpEach() {
    uint32_t const totalCount = history.totalCount();

    server.setContentLength(DumpHeaderSize + totalCount * sizeof(DumpRecord));
#ifdef PRIVATE_ALLOW_CROSS
    serverSendHeadersForRaw();
#endif
    if (totalCount > 0) {
      history.first(unixOffset);
    }
    memcpy(sharedBytesBuffer, "E82\1", 4); /* Magic "E82", dump format version 1 */
    memcpy(sharedBytesBuffer + 4, &totalCount, sizeof(totalCount));
    memcpy(sharedBytesBuffer + 8, &unixOffset, sizeof(unixOffset));
    server.send(200, "application/octet-stream", "", 0);
    server.sendContent(sharedStrBuffer, DumpHeaderSize);
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
  sender.flushBuffered();
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
