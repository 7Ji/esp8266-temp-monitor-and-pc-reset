/* This is included INSIDE a C++ struct/class body; DO NOT ADD ANY HEADER INCLUSION*/
  void recoverOnFailureFirst(uint16_t &sectorID, uint16_t &flashSectorID) {
    Serial.println(", consider this sector bad and expect next sector to be head of second part of ring");
    headL2 = 0;
    countL2 = sectorID << FlashStats::SectPageFactor;
    ++sectorID;
    ++flashSectorID;
  }

  /* true if the scan shall end, false for continue */
  bool recoverFlashFirst(uint16_t &sectorID, uint16_t &flashSectorID, uint64_t &unixFirst) {
    SpiFlashOpResult opResult;
    uint64_t unixThis, unixLast = 0;
    uint16_t sectorPageID, pageID, flashPageID;
    uint8_t emptyCount;

    Serial.println("Recovering first half");
    for (
      sectorID = 0,
      flashSectorID = FlashStats::SectStart;

      sectorID < FlashStats::SectTotal;

      ++sectorID,
      ++flashSectorID
    ) {
      for (
        sectorPageID = 0,
        pageID = sectorID << FlashStats::SectPageFactor,
        flashPageID = flashSectorID << FlashStats::SectPageFactor,
        emptyCount = 0;

        sectorPageID < FlashStats::SectPageCount;

        ++sectorPageID,
        ++pageID,
        ++flashPageID
      ) {
        opResult = spi_flash_read(flashPageID << FlashStats::PageExp, sliceL2Raw, FlashStats::PageSize);
        if (opResult != SPI_FLASH_RESULT_OK) {
          Serial.printf("Failed to read page %" PRIu16 "/f%" PRIu16 " when recovering first part of flash, using good pages before it\n", pageID, flashPageID);
          headL2 = 0;
          countL2 = sectorID << FlashStats::SectPageFactor;
          return true;
        }
        if (sliceL2.erased()) { /* Empty page */
          ++emptyCount;
          continue;
        }
        if (emptyCount) { /* Non-empty page after empty */
          Serial.printf("Non-empty page %" PRIu16 "/f%" PRIu16 " appears after empty page in same sector", pageID, flashPageID);
          recoverOnFailureFirst(sectorID, flashSectorID);
          return false;
        }
        if (!sliceL2.valid()) { /* Invalid page */
          Serial.printf("Invalid (wrong magic or checksum) page %" PRIu16 "/f%" PRIu16, pageID, flashPageID);
          recoverOnFailureFirst(sectorID, flashSectorID);
          return false;
        }
        unixThis = sliceL2.unixOffset;
        if (!unixFirst) {
          unixFirst = unixThis;
        }
        if (unixThis <= unixLast) { /* Jumping back, allowed only once, and only for the first page in sector */
          if (pageID & FlashStats::PageInSectMask) { /* Not first page in sector */
            Serial.printf("Page %" PRIu16 "/f%" PRIu16 " jump back and it's not the first page in sector", pageID, flashPageID);
            recoverOnFailureFirst(sectorID, flashSectorID);
            return false;
          } else { /* First page in sector, this is head of second part */
            Serial.printf("Page %" PRIu16 "/f%" PRIu16 " jump back and it's the first page in sector, consider this sector as head of second half of ring", pageID, flashPageID);
            headL2 = 0;
            countL2 = sectorID << FlashStats::SectPageFactor;
            return false;
          }
        }
        unixLast = unixThis;
      }
      if (emptyCount) { /* With empty page in current, next shall be head */
        headL2 = 0;
        countL2 = ((sectorID + 1) << FlashStats::SectPageFactor) - emptyCount;
        return false;
      }
    }
    headL2 = 0;
    countL2 = FlashStats::PageTotal;
    return true;
  }

  void recoverOnFailureSecond() {
    Serial.println(", consider second half of ring bad and use only first half of ring");
  }

  void recoverFlashSecond(uint16_t sectorID, uint16_t flashSectorID, uint64_t unixFirst) {
    SpiFlashOpResult opResult;
    uint64_t unixThis, unixLast = 0;
    uint16_t sectorPageID, pageID, flashPageID;
    uint16_t const sectorStart = sectorID;

    Serial.println("Recovering second half");
    for (
      ;
      sectorID < FlashStats::SectTotal;

      ++sectorID,
      ++flashSectorID
    ) {
      for (
        sectorPageID = 0,
        pageID = sectorID << FlashStats::SectPageFactor,
        flashPageID = flashSectorID << FlashStats::SectPageFactor;

        sectorPageID < FlashStats::SectPageCount;

        ++sectorPageID,
        ++pageID,
        ++flashPageID
      ) {
        opResult = spi_flash_read(flashPageID << FlashStats::PageExp, sliceL2Raw, FlashStats::PageSize);
        if (opResult != SPI_FLASH_RESULT_OK) {
          Serial.printf("Failed to read page %" PRIu16 "/f%" PRIu16 " when recovering second part of flash", pageID, flashPageID);
          recoverOnFailureSecond();
          return;
        }
        if (sliceL2.erased()) { /* Empty page */
          Serial.printf("Empty page %" PRIu16 "/f%" PRIu16, pageID, flashPageID);
          recoverOnFailureSecond();
          return;
        }
        if (!sliceL2.valid()) { /* Invalid page */
          Serial.printf("Invalid (wrong magic or checksum) page %" PRIu16 "/f%" PRIu16, pageID, flashPageID);
          recoverOnFailureSecond();
          return;
        }
        unixThis = sliceL2.unixOffset;
        if (!unixFirst) {
          unixFirst = unixThis;
        }
        if (unixThis <= unixLast) { /* Jumping back, not allowed in second half */
          Serial.printf("Page %" PRIu16 "/f%" PRIu16 " jump back", pageID, flashPageID);
          recoverOnFailureSecond();
          return;
        }
        unixLast = unixThis;
      }
    }
    if (unixFirst <= unixLast) {
      Serial.printf("Flash jumped back yet the first unix offset %" PRIu64 " is not larger than last offset %" PRIu64, unixFirst, unixLast);
      recoverOnFailureSecond();
      return;
    }
    headL2 = sectorStart;
    countL2 += (FlashStats::SectTotal - sectorStart) << FlashStats::SectPageFactor;
  }

  void recoverFlash() {
    uint16_t sectorID, flashSectorID;
    uint64_t unixFirst = 0;

    Serial.println("Recovering on-flash records...");
    if (recoverFlashFirst(sectorID, flashSectorID, unixFirst)) {
      return;
    }
    Serial.printf("First half has %" PRIu16 " good pages\n", countL2);
    if (sectorID < FlashStats::SectTotal) {
      recoverFlashSecond(sectorID, flashSectorID, unixFirst);
    }
  }
