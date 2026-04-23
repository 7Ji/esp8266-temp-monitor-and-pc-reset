#pragma once

#include <cstdint>

#include "flashStats.h"
#include "printer.h"
#include "sensorPage.h"
#include "sharedBuffer.h"

namespace RecoverFlash {
  static inline constexpr uint16_t WrittenBit = 0b1;
  struct PagesCandidate {
    uint64_t clockBegin, clockEnd;
    uint16_t head, count;

    [[gnu::always_inline]] inline void maybeReplace(PagesCandidate const &other) {
      if (other.clockEnd > clockEnd) {
        *this = other;
      }
    }

    [[gnu::always_inline]] inline void maybeReplaceBeforeSector(PagesCandidate &other, uint16_t const sectorID) {
      if (sectorID > other.head && other.clockEnd > clockEnd) {
        other.count = (sectorID - other.head) << FlashStats::SectPageFactor;
        *this = other;
      }
    }

    [[gnu::always_inline]] inline void init(uint16_t const sectorID) {
      head = sectorID;
      count = 0;
      clockBegin = 0;
      clockEnd = 0;
    }
  };

  bool readFlashSector(uint16_t flashSectorID);

  [[gnu::always_inline]] inline void recoverPages(PagesCandidate &best, PagesCandidate &current, uint16_t &sectorID, uint16_t &flashSectorID) {
    static SensorPage const *SharedPagesBuffer = reinterpret_cast<SensorPage const *>(sharedBytesBuffer);

    SensorPage const *page;
    uint64_t unixThis, unixLast = 0;
    uint32_t *pageWordsBuffer;
    uint16_t pageID, flashPageID, writtenMask;
    uint8_t sectorPageID, pageWordID, emptyCount;
    current.init(sectorID);

    for (
      ;
      sectorID < FlashStats::SectTotal;

      ++sectorID,
      ++flashSectorID
    ) {
      if (!readFlashSector(flashSectorID)) {
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
        page = SharedPagesBuffer;

        sectorPageID < FlashStats::SectPageCount;

        ++sectorPageID,
        ++pageID,
        ++flashPageID,
        ++page
      ) {
        if (!(writtenMask & (WrittenBit << sectorPageID))) {
          ++emptyCount;
          continue;
        }
        if (emptyCount) { /* Non-empty page after empty */
          PRINTF("Non-empty page %" PRIu16 "/f%" PRIu16 " appears after empty page in same sector\n", pageID, flashPageID);
          best.maybeReplaceBeforeSector(current, sectorID++);
          ++flashSectorID;
          return;
        }
        if (!page->valid()) { /* Invalid page */
          PRINTF("Invalid page %" PRIu16 "/f%" PRIu16 "\n", pageID, flashPageID);
          best.maybeReplaceBeforeSector(current, sectorID++);
          ++flashSectorID;
          return;
        }
        unixThis = page->unixOffset;
        if (!current.clockBegin) {
          current.clockBegin = unixThis;
        }
        if (unixThis <= unixLast) { /* Jumping back, allowed only once, and only for the first page in sector */
          if (pageID & FlashStats::PageInSectMask) { /* Not first page in sector */
            PRINTF("Page %" PRIu16 "/f%" PRIu16 " jump back and it's not the first page in sector\n", pageID, flashPageID);
            best.maybeReplaceBeforeSector(current, sectorID++);
            ++flashSectorID;
            return;
          } else { /* First page in sector, this is head of second part */
            PRINTF("Page %" PRIu16 "/f%" PRIu16 " jump back and it's the first page in sector, consider this sector as head of next chain of pages\n", pageID, flashPageID);
            best.maybeReplaceBeforeSector(current, sectorID);
            return;
          }
        }
        unixLast = unixThis + page->records[SensorPage::MaxRecordsSub1].timestamp;
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

  [[gnu::always_inline]] inline void recoverFlash(uint16_t &headL2, uint16_t &countL2) {
    uint16_t sectorID = 0, flashSectorID = FlashStats::SectStart, countFirst = 0;
    PagesCandidate best = {}, last = {};
    uint64_t clockBeginFirst = 0;

    PRINTLN("Recovering on-flash records...");

    /* The pages at head are special, they could be the tail-wrapped-around part of a ring */
    recoverPages(best, last, sectorID, flashSectorID);
    if (best.count) {
      countFirst = best.count;
      clockBeginFirst = best.clockBegin;
    }
    while (sectorID < FlashStats::SectTotal) {
      recoverPages(best, last, sectorID, flashSectorID);
    }
    /* Head and tail candidates may be two halves of one wrapped ring */
    if (!best.head &&
      last.head &&
      (last.head << FlashStats::SectPageFactor) + last.count == FlashStats::PageTotal &&
      clockBeginFirst > last.clockEnd) {
      PRINTLN("Recovered wrapped ring");
      headL2 = last.head;
      countL2 = last.count + countFirst;
    } else {
      headL2 = best.head;
      countL2 = best.count;
    }
    PRINTF("Recovered %" PRIu16 " pages from flash, head is %" PRIu16 "\n", countL2, headL2);
  }
}
