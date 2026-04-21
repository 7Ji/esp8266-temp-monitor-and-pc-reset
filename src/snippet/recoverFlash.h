#pragma once
#ifndef PRINTER
#define PRINTER Serial.
#endif
/* This is included INSIDE a C++ struct/class body; DO NOT ADD ANY HEADER INCLUSION*/
  struct SlicesCandidate {
    uint64_t clockBegin, clockEnd;
    uint16_t head, count;

    void maybeReplace(SlicesCandidate const &other) {
      if (other.clockEnd > clockEnd) {
        *this = other;
      }
    }

    void maybeReplaceBeforeSector(SlicesCandidate &other, uint16_t const sectorID) {
      if (sectorID > other.head && other.clockEnd > clockEnd) {
        other.count = (sectorID - other.head) << FlashStats::SectPageFactor;
        *this = other;
      }
    }

    void init(uint16_t const sectorID) {
      head = sectorID;
      count = 0;
      clockBegin = 0;
      clockEnd = 0;
    }
  };

  void recoverSlices(SlicesCandidate &best, SlicesCandidate &current, uint16_t &sectorID, uint16_t &flashSectorID) {
    static SensorSlice const *SharedSlicesBuffer = reinterpret_cast<SensorSlice const *>(sharedBytesBuffer);
    static constexpr uint16_t WrittenBit = 0b1;

    SensorSlice const *slice;
    SpiFlashOpResult opResult;
    uint64_t unixThis, unixLast = 0;
    uint32_t *pageWordsBuffer;
    uint16_t pageID, flashPageID, writtenMask;
    uint8_t sectorPageID, pageWordID, emptyCount, *sliceRaw;
    current.init(sectorID);

    for (
      ;
      sectorID < FlashStats::SectTotal;

      ++sectorID,
      ++flashSectorID
    ) {
      opResult = spi_flash_read(flashSectorID << FlashStats::SectExp, sharedWordsBuffer, FlashStats::SectSize);
      if (opResult != SPI_FLASH_RESULT_OK) {
        PRINTER printf("Failed to read sector %" PRIu16 "/f%" PRIu16 " when recovering flash\n", sectorID, sectorID);
        best.maybeReplaceBeforeSector(current, sectorID++);
        ++flashSectorID;
        return;
      }
      writtenMask = 0;
      for (
        sectorPageID = 0,
        pageWordsBuffer = sharedWordsBuffer;

        sectorPageID < FlashStats::SectPageCount;

        ++sectorPageID,
        pageWordsBuffer += FlashStats::PageWordCount
      ) {
        for (pageWordID = 0; pageWordID < FlashStats::PageWordCount; ++pageWordID) {
          if (pageWordsBuffer[pageWordID] != UINT32_MAX) {
            writtenMask |= (WrittenBit << sectorPageID);
            break;
          }
        }
      }
      if (!writtenMask) { /* An erased sector that's not written yet */
        if (sectorID == current.head && sectorID > 0 && sectorID < FlashStats::SectTotal - 1) { /* Just scanning first sector, step without return, but not for flash begining or end */
          current.init(sectorID + 1);
          continue;
        }
        best.maybeReplaceBeforeSector(current, sectorID++);
        ++flashSectorID;
        return;
      }
      for (
        sectorPageID = 0,
        pageID = sectorID << FlashStats::SectPageFactor,
        flashPageID = flashSectorID << FlashStats::SectPageFactor,
        emptyCount = 0,
        slice = SharedSlicesBuffer,
        sliceRaw = sharedBytesBuffer;

        sectorPageID < FlashStats::SectPageCount;

        ++sectorPageID,
        ++pageID,
        ++flashPageID,
        ++slice,
        sliceRaw += FlashStats::PageSize
      ) {
        if (!(writtenMask & (WrittenBit << sectorPageID))) {
          ++emptyCount;
          continue;
        }
        if (emptyCount) { /* Non-empty page after empty */
          PRINTER printf("Non-empty page %" PRIu16 "/f%" PRIu16 " appears after empty page in same sector\n", pageID, flashPageID);
          best.maybeReplaceBeforeSector(current, sectorID++);
          ++flashSectorID;
          return;
        }
        if (!slice->valid()) { /* Invalid page */
          PRINTER printf("Invalid page %" PRIu16 "/f%" PRIu16 "\n", pageID, flashPageID);
          best.maybeReplaceBeforeSector(current, sectorID++);
          ++flashSectorID;
          return;
        }
        unixThis = slice->unixOffset;
        if (!current.clockBegin) {
          current.clockBegin = unixThis;
        }
        if (unixThis <= unixLast) { /* Jumping back, allowed only once, and only for the first page in sector */
          if (pageID & FlashStats::PageInSectMask) { /* Not first page in sector */
            PRINTER printf("Page %" PRIu16 "/f%" PRIu16 " jump back and it's not the first page in sector\n", pageID, flashPageID);
            best.maybeReplaceBeforeSector(current, sectorID++);
            ++flashSectorID;
            return;
          } else { /* First page in sector, this is head of second part */
            PRINTER printf("Page %" PRIu16 "/f%" PRIu16 " jump back and it's the first page in sector, consider this sector as head of next chain of slices\n", pageID, flashPageID);
            best.maybeReplaceBeforeSector(current, sectorID);
            return;
          }
        }
        unixLast = unixThis + slice->records[SensorSlice::MaxRecordsSub1].timestamp;
      }
      current.clockEnd = unixLast; /* Only updated per sector */
      if (emptyCount) { /* With empty page in current, next shall be head */
        current.count = ((sectorID - current.head + 1) << FlashStats::SectPageFactor) - emptyCount;
        if (current.count) {
          best.maybeReplace(current);
        }
        ++sectorID;
        ++flashSectorID;
        return;
      }
    }
    current.count = (sectorID - current.head) << FlashStats::SectPageFactor;
    best.maybeReplace(current);
  }

  void recoverFlash() {
    uint16_t sectorID = 0, flashSectorID = FlashStats::SectStart, countFirst = 0;
    SlicesCandidate best = {}, last = {};
    uint64_t clockBeginFirst = 0;

    PRINTER println("Recovering on-flash records...");

    /* The slices at head is special, they could be the tail-wrapped-around part of a ring */
    recoverSlices(best, last, sectorID, flashSectorID);
    if (best.count) {
      countFirst = best.count;
      clockBeginFirst = best.clockBegin;
    }
    while (sectorID < FlashStats::SectTotal) {
      recoverSlices(best, last, sectorID, flashSectorID);
    }
    /* Head and tail candidates may be two halves of one wrapped ring */
    if (!best.head &&
      last.head &&
      (last.head << FlashStats::SectPageFactor) + last.count == FlashStats::PageTotal &&
      clockBeginFirst > last.clockEnd) {
      PRINTER println("Recovered wrapped ring");
      headL2 = last.head;
      countL2 = last.count + countFirst;
    } else {
      headL2 = best.head;
      countL2 = best.count;
    }
    PRINTER printf("Recovered %" PRIu16 " slices from flash, head is %" PRIu16 "\n", countL2, headL2);
  }
