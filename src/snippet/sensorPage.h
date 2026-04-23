#pragma once

#include <cstdint>

#include "printer.h"

struct SensorValue {
  static inline constexpr int lenBufferTempOrHumid = 8; /* extreme 255.255\0 */
  static inline constexpr int lenBufferTempAndHumid = lenBufferTempOrHumid * 2; /* extreme 255.255,255.255\0 */
  static inline constexpr int lenBuffer = lenBufferTempAndHumid + 22; /* extreme 18446744073709551615,255.255,255.255\n\0 */

  int8_t tempInt;
  uint8_t tempDot;
  uint8_t humidInt;
  uint8_t humidDot;

  /* buffer should be at least lenBuffer */
  bool intoStr(uint64_t const unixSeconds, char *const buffer, std::size_t &len) const;
};

static_assert(sizeof(SensorValue) == 4, "SensorValue should have a size of 4");

struct SensorRecord {
  uint16_t timestamp; /* second offset from page unixOffset */
  SensorValue value;
};

static_assert(sizeof(SensorRecord) == 6, "SensorRecord should have a size of 6");

struct SensorPage {
  static inline constexpr uint32_t Magic = 0x82660C05;
  static inline constexpr uint8_t MaxRecords = 40;
  static inline constexpr uint8_t MaxRecordsSub1 = MaxRecords - 1;
  static inline constexpr uint8_t CountChecksum32 = (sizeof(uint64_t) + MaxRecords * sizeof(SensorRecord)) / sizeof(uint32_t);
  static inline constexpr uint8_t CountAll32 = CountChecksum32 + 2;

  uint32_t magic = Magic;
  uint32_t checksum;
  uint64_t unixOffset;
  SensorRecord records[MaxRecords];

  uint32_t actualChecksum() const;

  void shift() {
    uint8_t recordID, recordIDNext, diff;

    records[0] = records[1];
    records[0].timestamp = 0;

    diff = records[1].timestamp;
    unixOffset += diff;

    for (recordID = 1; recordID < MaxRecordsSub1;) {
      recordIDNext = recordID + 1;
      records[recordID] = records[recordIDNext];
      records[recordID].timestamp -= diff;
      recordID = recordIDNext;
    }
  }

  bool valid() const {
    uint32_t expectedChecksum;
    uint16_t timestampLast = 0, timestampThis;
    uint8_t recordID;

    if (magic != SensorPage::Magic) {
      PRINTF("Page magic not right (recorded %08" PRIx32 " != expected %08" PRIx32 ")\n", magic, Magic);
      return false;
    }
    expectedChecksum = actualChecksum();
    if (checksum != expectedChecksum) {
      PRINTF("Page Checksum mismatch (recorded %08" PRIx32 " != expected %08" PRIx32")\n", checksum, expectedChecksum);
      return false;
    }
    if (records[0].timestamp != 0) {
      PRINTLN ("Page first record timestamp is not 0");
      return false;
    }
    for (recordID = 1; recordID < MaxRecords; ++recordID) {
      timestampThis = records[recordID].timestamp;
      if (timestampThis <= timestampLast) {
        PRINTF ("Page record %" PRIu8 " timestamp jump back (%" PRIu16 " <= %" PRIu16 ")", recordID, timestampThis, timestampLast);
        return false;
      }
      timestampLast = timestampThis;
    }
    return true;
  }
};
#undef PRINTER
