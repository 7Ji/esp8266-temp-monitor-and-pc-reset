#pragma once

#include <stdint.h>
#include <stddef.h>

struct RecoverFlashConfig {
  uint16_t flashSectStart;
  uint16_t flashSectTotal;
  uint8_t flashSectPageFactor;
  uint8_t flashSectPageCount;
  uint8_t flashPageExp;
  uint16_t firstPageCountEmpty;
};

struct RecoverFlashResult {
  uint16_t headL2 = 0;
  uint16_t countL2 = 0;
};

typedef bool (*RecoverFlashReadPageFn)(void *ctx, uint16_t flashPageID);
typedef bool (*RecoverFlashIsErasedFn)(void *ctx);
typedef bool (*RecoverFlashIsValidFn)(void *ctx);
typedef uint64_t (*RecoverFlashUnixOffsetFn)(void *ctx);

template <typename Logger>
RecoverFlashResult recoverFlashLayout(
  RecoverFlashConfig const &config,
  void *ctx,
  RecoverFlashReadPageFn readPage,
  RecoverFlashIsErasedFn isErased,
  RecoverFlashIsValidFn isValid,
  RecoverFlashUnixOffsetFn unixOffsetOf,
  Logger log
) {
  uint16_t sectorID, flashSectorID, sectorPageID, flashPageID, pageCount = 0, firstPageCount = 0, secondSectHead = 0;
  uint64_t unixLast = 0, unixLastSector = 0, unixThis, unixFirst = 0;
  bool first = true, seenEmptyPage;
  RecoverFlashResult result = {};

  log("Recovering on-flash records...");
  for (
    sectorID = 0,
    flashSectorID = config.flashSectStart;

    sectorID < config.flashSectTotal;

    ++sectorID,
    ++flashSectorID
  ) {
    for (
      sectorPageID = 0,
      flashPageID = flashSectorID << config.flashSectPageFactor,
      seenEmptyPage = false;

      sectorPageID < config.flashSectPageCount;

      ++sectorPageID,
      ++flashPageID
    ) {
      if (!readPage(ctx, flashPageID)) {
        log("Read failed when recovering, using good pages before it");
        result.countL2 = firstPageCount;
        return result;
      }
      if (isErased(ctx)) {
        if (secondSectHead > 0) {
          log("Empty page where wrapped second head was expected, use only first half");
          result.countL2 = firstPageCount;
          return result;
        }
        seenEmptyPage = true;
        continue;
      }
      if (seenEmptyPage || !isValid(ctx)) {
        if (secondSectHead > 0) {
          log("Bad page in expected second half, using sectors before wrap");
          result.countL2 = firstPageCount;
          return result;
        } else {
          log("Bad sector, expect next sector to be head of second part");
          firstPageCount = sectorID << config.flashSectPageFactor;
          secondSectHead = sectorID + 1;
          unixLast = unixLastSector;
          break;
        }
      }
      unixThis = unixOffsetOf(ctx);
      if (first) {
        unixFirst = unixThis;
        first = false;
      }
      if (unixThis <= unixLast) {
        if (secondSectHead > 0 && (sectorID != secondSectHead || sectorPageID > 0)) {
          log("Unexpected jump back after wrapped head expectation, use pages before it");
          result.countL2 = firstPageCount;
          return result;
        } else if (sectorPageID > 0) {
          log("Jump back not at first page, consider sector bad and expect next sector");
          firstPageCount = sectorID << config.flashSectPageFactor;
          secondSectHead = sectorID + 1;
          unixLast = unixLastSector;
          break;
        } else {
          if (!secondSectHead) {
            firstPageCount = sectorID << config.flashSectPageFactor;
            secondSectHead = sectorID;
          }
        }
      } else if (!secondSectHead) {
        ++firstPageCount;
      }
      ++pageCount;
      unixLast = unixThis;
    }
    if (seenEmptyPage && !secondSectHead) {
      secondSectHead = sectorID + 1;
    }
    if (sectorPageID == config.flashSectPageCount) {
      unixLastSector = unixLast;
    }
  }
  if (secondSectHead > 0 && unixFirst <= unixLast) {
    log("Wrap detected but first unix offset is not larger than last, use pages before wrap");
    result.countL2 = firstPageCount;
  } else {
    result.headL2 = secondSectHead;
    result.countL2 = pageCount;
  }
  return result;
}
