#pragma once
#ifndef PRINTER
#define PRINTER Serial.
#endif
struct SensorPage {
  COMPCONST uint32_t const Magic = 0x82660C05;
  COMPCONST uint8_t const MaxRecords = 40;
  COMPCONST uint8_t const MaxRecordsSub1 = MaxRecords - 1;
  COMPCONST uint8_t const CountChecksum32 = (sizeof(uint64_t) + MaxRecords * sizeof(SensorRecord)) / sizeof(uint32_t);
  COMPCONST uint8_t const CountAll32 = CountChecksum32 + 2;

  uint32_t magic = Magic;
  uint32_t checksum;
  uint64_t unixOffset;
  SensorRecord records[MaxRecords];

  uint32_t actualChecksum() const {
    return crc32(&unixOffset, sizeof(unixOffset) + sizeof(records));
  }

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
      PRINTER printf("Page magic not right (recorded %08" PRIx32 " != expected %08" PRIx32 ")\n", magic, SensorPage::Magic);
      return false;
    }
    expectedChecksum = actualChecksum();
    if (checksum != expectedChecksum) {
      PRINTER printf("Page Checksum mismatch (recorded %08" PRIx32 " != expected %08" PRIx32")\n", checksum, expectedChecksum);
      return false;
    }
    if (records[0].timestamp != 0) {
      PRINTER println("Page first record timestamp is not 0");
      return false;
    }
    for (recordID = 1; recordID < MaxRecords; ++recordID) {
      timestampThis = records[recordID].timestamp;
      if (timestampThis <= timestampLast) {
        PRINTER printf("Page record %" PRIu8 " timestamp jump back (%" PRIu16 " <= %" PRIu16 ")", recordID, timestampThis, timestampLast);
        return false;
      }
      timestampLast = timestampThis;
    }
    return true;
  }
};
#undef PRINTER
