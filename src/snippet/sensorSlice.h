#pragma once
#ifndef PRINTER
#define PRINTER Serial.
#endif
struct SensorSlice {
  COMPCONST uint8_t const Magic[3] = {'E', '8', '2'};
  COMPCONST uint8_t const MaxRecords = 40;
  COMPCONST uint8_t const MaxRecordsSub1 = MaxRecords - 1;

  SensorRecord records[MaxRecords];
  uint64_t unixOffset;
  uint8_t count = 0;
  uint8_t magic[3] = {'E', '8', '2'};
  uint32_t checksum = 0;

  uint32_t actualChecksum() const {
    return crc32(this, sizeof(*this) - sizeof(checksum));
  }

  void clear() {
    memset(records, 0, sizeof(records));
    unixOffset = 0;
    count = 0;
    memcpy(magic, Magic, sizeof(magic));
    checksum = 0;
  }

  void finalize(uint8_t const countNext) {
    count = countNext;
    if (count < MaxRecords) {
      memset(records + count, 0, sizeof(SensorRecord) * (MaxRecords - count));
    }
    memcpy(magic, Magic, sizeof(magic));
    checksum = actualChecksum();
  }

  void shift() {
    uint8_t recordID, recordIDNext, diff;

    if (count < 2) {
      clear();
      return;
    }

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
    memset(records + MaxRecordsSub1, 0, sizeof(SensorRecord));
    count = MaxRecordsSub1;
  }

  bool valid() const {
    uint32_t expectedChecksum;
    uint16_t timestampLast = 0, timestampThis;
    uint8_t recordID;
    SensorRecord const EmptyRecord = {};

    if (memcmp(magic, Magic, sizeof(magic))) {
      PRINTER println("Slice magic not right");
      return false;
    }
    if (count < 1 || count > MaxRecords) {
      PRINTER printf("Slice count invalid (%" PRIu8 ")\n", count);
      return false;
    }
    expectedChecksum = actualChecksum();
    if (checksum != expectedChecksum) {
      PRINTER printf("Slice Checksum mismatch (recorded %08" PRIx32 " != expected %08" PRIx32")\n", checksum, expectedChecksum);
      return false;
    }
    if (records[0].timestamp != 0) {
      PRINTER println("Slice first record timestamp is not 0");
      return false;
    }
    for (recordID = 1; recordID < MaxRecords; ++recordID) {
      timestampThis = records[recordID].timestamp;
      if (timestampThis <= timestampLast) {
        PRINTER printf("Slice record %" PRIu8 " timestamp jump back (%" PRIu16 " <= %" PRIu16 ")", recordID, timestampThis, timestampLast);
        return false;
      }
      timestampLast = timestampThis;
    }
    for (; recordID < MaxRecords; ++recordID) {
      if (memcmp(records + recordID, &EmptyRecord, sizeof(EmptyRecord))) {
        PRINTER printf("Slice record %" PRIu8 " is not empty beyond count %" PRIu8 "\n", recordID, count);
        return false;
      }
    }
    return true;
  }
};
#undef PRINTER
