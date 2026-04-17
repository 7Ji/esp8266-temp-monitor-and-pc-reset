/* This is included INSIDE a C++ struct/class body; DO NOT ADD ANY HEADER INCLUSION*/

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
        if (sliceL2.erased()) { /* Empty page */
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
        Serial.printf("Empty pages were seen in sector %" PRIu16 "/f%" PRIu16 ", expecting next sector to be head\n", sectorID, flashSectorID);
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
