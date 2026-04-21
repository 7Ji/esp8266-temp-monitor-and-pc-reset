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
COMPCONST size_t const SharedBufferSize = 4096;
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
static byte _sharedBuffer[SharedBufferSize];
static byte *const sharedBytesBuffer = _sharedBuffer;
static char *const sharedStrBuffer = reinterpret_cast<char *>(sharedBytesBuffer);

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

  void maybeReconnect(uint32_t const millisCurrent) {
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

  void maybeUpdate(uint32_t const millisCurrent) {
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

#include "snippet/sensorSlice.h"
#include "snippet/flashStats.h"

struct SensorHistory {
  COMPCONST uint32_t const MinInterval = 2000;
  COMPCONST uint8_t const MaxHistoryL0 = 1 << 5; /* 2s * 32, even secondly, for about a minute */

  COMPCONST uint8_t const RingMaskL0 = MaxHistoryL0 - 1;

  static_assert(sizeof(struct SensorSlice) == FlashStats::PageSize, "SensorSlice should have the same size as page");

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
    return headL2 << FlashStats::SectPageFactor;
  }

  uint16_t slicePage(uint16_t const sliceID) const {
    return (firstPage() + sliceID) % FlashStats::PageTotal;
  }

  uint32_t totalCount() const {
    return uint32_t(countL2) * SensorSlice::MaxRecords + countL1 + countL0;
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
    uint16_t const flashSectorID = sectorID + FlashStats::SectStart;
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
    uint16_t const flashPageID = pageID + FlashStats::PageStart;
    SpiFlashOpResult opResult;

    Serial.printf("Writing L1 to page %" PRIu16 "/f%" PRIu16 "\n", pageID, flashPageID);
    opResult = spi_flash_write(flashPageID * FlashStats::PageSize, sliceL1Raw, FlashStats::PageSize);
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
    sliceL1.checksum = sliceL1.actualChecksum();
    if (!writeL1(pageID)) {
      fallbackShift();
      return;
    }
    countL1 = 0;
    ++countL2;
  }

  bool fetchFlashPage(uint16_t const flashPageID) {
    SpiFlashOpResult opResult;

    opResult = spi_flash_read(flashPageID * FlashStats::PageSize, sliceL2Raw, FlashStats::PageSize);

    if (opResult != SPI_FLASH_RESULT_OK) {
      Serial.printf("fetch f%" PRIu16 ": Read not OK, op result %d\n", flashPageID, opResult);
      return false;
    }
    return sliceL2.valid();
  }

  bool fetchPage(uint16_t const pageID) {
    return fetchFlashPage(pageID + FlashStats::PageStart);
  }
#include "snippet/recoverFlash.h"

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

struct RawAllSender {
  COMPCONST size_t const lenBuffer = SharedBufferSize;
  COMPCONST size_t const lenMax = SharedBufferSize - SensorValue::lenBuffer;

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
    if (value.intoStr(unixSeconds, sharedStrBuffer + offset, len)) {
      if ((offset += len) >= lenMax) {
        server.sendContent(sharedStrBuffer, offset);
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
    uint16_t i, pageOffset;

    if (history.countL2 > 0) {
      pageOffset = history.firstPage();
      if (pageOffset > 0) {
        for (i = pageOffset; i < FlashStats::PageTotal; ++i) {
          sendPage(i);
        }
        pageOffset = history.countL2 + pageOffset - FlashStats::PageTotal;
        for (i = 0; i < pageOffset; ++i) {
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
      server.sendContent(sharedStrBuffer, offset);
    }
    server.sendContent("", 0);
  }
};

void httpHandleHistory() {
  RawAllSender sender = {};

  sender.sendAll();
}

struct DumpAllSender {
  COMPCONST size_t const lenBuffer = SharedBufferSize;
  COMPCONST size_t const lenMax = SharedBufferSize - sizeof(DumpRecord);

  uint64_t unixOffset = 0;
  size_t offset = 0;

  DumpAllSender() {
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
    uint64_t const sliceUnixOffset = slice.unixOffset;
    SensorRecord const *record;
    uint16_t i;

    for (i = 0; i < count; ++i) {
      record = slice.records + i;
      sendValue(sliceUnixOffset + record->timestamp, record->value);
    }
  }

  void sendPage(uint16_t const pageID) {
    if (!history.fetchPage(pageID)) {
      return;
    }
    sendSlice(history.sliceL2);
  }

  void sendAll() {
    uint16_t i, pageOffset;

    if (history.countL2 > 0) {
      pageOffset = history.firstPage();
      if (pageOffset > 0) {
        for (i = pageOffset; i < FlashStats::PageTotal; ++i) {
          sendPage(i);
        }
        pageOffset = history.countL2 + pageOffset - FlashStats::PageTotal;
        for (i = 0; i < pageOffset; ++i) {
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

  ~DumpAllSender() {
    if (offset) {
      server.sendContent(sharedStrBuffer, offset);
    }
  }
};

void httpHandleDump() {
  DumpAllSender sender = {};

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
  history.recoverFlash();

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
