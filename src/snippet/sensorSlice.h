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

  bool erased() {
    uint32_t const *const raw = reinterpret_cast<uint32_t const *>(this);
    uint16_t wordID;

    for (wordID = 0; wordID < CountAll32; ++ wordID) {
      if (raw[wordID] != UINT32_MAX) {
        return false;
      }
    }
    return true;
  }

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
