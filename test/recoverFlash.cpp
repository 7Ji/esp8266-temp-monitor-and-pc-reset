#include <vector>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <unistd.h>

#include "snippet/flashStats.h"

typedef int SpiFlashOpResult;
COMPCONST SpiFlashOpResult SPI_FLASH_RESULT_OK = 0;

namespace Printer {
  using std::printf;
  static constexpr auto println = std::puts;
};

static uint8_t buffer[FlashStats::AddrEnd];

static size_t ReadFailureOffset = SIZE_MAX;

struct ReadFailure {
  ReadFailure(uint16_t const pageID) {
    ReadFailureOffset = FlashStats::AddrStart + pageID * FlashStats::PageSize;
  }
  ~ReadFailure() {
    ReadFailureOffset = SIZE_MAX;
  }
};

SpiFlashOpResult spi_flash_read(size_t const offset, void *const target, size_t const size) {
  if (offset == ReadFailureOffset) {
    return 1;
  }
  std::memcpy(target, buffer + offset, size);
  return SPI_FLASH_RESULT_OK;
}

struct SensorValue {
  int8_t tempInt;
  uint8_t tempDot;
  uint8_t humidInt;
  uint8_t humidDot;
};

struct SensorRecord {
  uint16_t timestamp;
  SensorValue value;
};

static uint32_t crc32 (void const* const data, size_t length, uint32_t crc = 0xffffffff)
{
    uint8_t const* ldata = (uint8_t const*)data;
    while (length--)
    {
        uint8_t c = *(ldata++);
        for (uint32_t i = 0x80; i > 0; i >>= 1)
        {
            bool bit = crc & 0x80000000;
            if (c & i)
                bit = !bit;
            crc <<= 1;
            if (bit)
                crc ^= 0x04c11db7;
        }
    }
    return crc;
}

#define PRINTER Printer::
#include "snippet/sensorSlice.h"

struct SensorHistory {
  union {
    SensorSlice sliceL2; /* about minutely, flush to flash every 30 minutes */
    uint32_t sliceL2Raw[sizeof(SensorSlice) / sizeof(uint32_t)];
  };
  uint16_t countL2 = 0; /* page */
  uint16_t headL2 = 0; /* sector */

#define PRINTER Printer::
#include "snippet/recoverFlash.h"
};

static SensorHistory history = {};

struct Counter {
  size_t pass = 0;
  size_t total = 0;

  void expect(char const *const title, uint16_t const count, uint16_t const head) {
    bool const cond = count == history.countL2 && head == history.headL2;
    pass += cond;
    ++total;
    std::printf("%s%03zu: %-40s: %s\n", cond ? "\033[32m" : "\033[31m", total, title, cond ? "PASS\033[0m" : "FAIL");
    if (!cond) {
      std::printf(" - countL2 expected %hu got %hu, headL2 expected %hu got %hu\033[0m\n", count, history.countL2, head, history.headL2);
    }
  }

  size_t sum() const {
    size_t const fail = total - pass;
    std::printf("Total: %zu, Pass: %zu, Fail: %zu\n", total, pass, fail);
    return fail;
  }
};

struct PagePlan {
  uint16_t offset, count;
  bool noMagic = false;
  bool noChecksum = false;
  bool badFirstTimestamp = false;
  bool badRecordOrder = false;
  uint64_t unixOffset = 0; /* When not zero, forcing this (and later to use this offset) */
};

void fillPages(std::vector<PagePlan> const &plans) {
  static void *const bufferSlices = buffer + FlashStats::AddrStart;
  static uint32_t const bufferSlicesSize = FlashStats::AddrEnd - FlashStats::AddrStart;
  static SensorSlice * const slicesAll = reinterpret_cast<SensorSlice *>(bufferSlices);

  memset(bufferSlices, 0xFF, bufferSlicesSize);

  uint64_t unixOffset = 1;
  for (PagePlan const &plan: plans) {
    SensorSlice *const slices = slicesAll + plan.offset;
    if (plan.unixOffset) {
      unixOffset = plan.unixOffset;
    }
    for (auto i = 0; i < plan.count; ++i) {
      SensorSlice *slice = slices + i;
      slice->magic = plan.noMagic ? 0 : slice->Magic;
      slice->unixOffset = unixOffset;
      memset(slice->records, 9, sizeof(slice->records)); /* temp 9.9, humid 9.9 */
      for (auto j = 0; j < SensorSlice::MaxRecords; ++j) {
        slice->records[j].timestamp = j;
      }
      if (plan.badFirstTimestamp) {
        slice->records[0].timestamp = 1;
      }
      if (plan.badRecordOrder) {
        slice->records[SensorSlice::MaxRecordsSub1].timestamp = slice->records[SensorSlice::MaxRecordsSub1 - 1].timestamp;
      }
      slice->checksum = plan.noChecksum ? 0 : slice->actualChecksum();
      unixOffset += 3600;
    }
  }
}

COMPCONST uint16_t const SectorsFirstHalf = FlashStats::SectTotal / 2;
COMPCONST uint16_t const PagesFirstHalf = SectorsFirstHalf << FlashStats::SectPageFactor;
COMPCONST uint16_t const PagesFirstHalfSubSect = PagesFirstHalf - FlashStats::SectPageCount;
COMPCONST uint16_t const PagesSecondHalf = FlashStats::PageTotal - PagesFirstHalf;

int main() {
  Counter counter = {};
  /* Empty Flash */
  fillPages({});
  history.recoverFlash();
  counter.expect("Empty Flash", 0, 0);
  /* First page */
  fillPages({{0, 1}});
  history.recoverFlash();
  counter.expect("First page", 1, 0);
  /* First 15 page */
  fillPages({{0, 15}});
  history.recoverFlash();
  counter.expect("First 15 pages", 15, 0);
  /* First 16 pages */
  fillPages({{0, 16}});
  history.recoverFlash();
  counter.expect("First 16 pages", 16, 0);
  /* First 17 pages */
  fillPages({{0, 17}});
  history.recoverFlash();
  counter.expect("First 17 pages", 17, 0);
  /* No magic at second sector head */
  fillPages({{0, 16}, {16, 1, .noMagic = true}});
  history.recoverFlash();
  counter.expect("No magic at second sector head", 16, 0);
  /* No checksum at second sector head */
  fillPages({{0, 16}, {16, 1, .noChecksum = true}});
  history.recoverFlash();
  counter.expect("No checksum at second sector head", 16, 0);
  /* Jump back at second sector head */
  fillPages({{0, 16}, {16, 1, .unixOffset = 1}});
  history.recoverFlash();
  counter.expect("Jump back at second sector head", 16, 0);
  /* Read failure at second sector head */
  fillPages({{0, 32}});
  {
    auto failure = ReadFailure(16);
    history.recoverFlash();
  }
  counter.expect("Read failure at second sector head", 16, 0);
  /* Non-empty page after empty page in same sector */
  fillPages({{0, 16}, {17, 1}});
  history.recoverFlash();
  counter.expect("Non-empty page after empty page in same sector", 16, 0);
  /* Invalid timestamp at second sector head */
  fillPages({{0, 16}, {16, 1, .badFirstTimestamp = true}});
  history.recoverFlash();
  counter.expect("Invalid timestamp at second sector head", 16, 0);
  /* Record timestamp jump back in second sector head */
  fillPages({{0, 16}, {16, 1, .badRecordOrder = true}});
  history.recoverFlash();
  counter.expect("Record timestamp jump back in second sector head", 16, 0);
  /* Jump back at second page in second sector */
  fillPages({{0, 18}, {17, 1, .unixOffset = 1}});
  history.recoverFlash();
  counter.expect("Jump back at second page in second sector", 16, 0);
  /* Whole flash */
  fillPages({{0, FlashStats::PageTotal}});
  history.recoverFlash();
  counter.expect("Whole Flash", FlashStats::PageTotal, 0);
  /* Whole flash but last no magic */
  fillPages({{0, FlashStats::PageTotal - 1}, {FlashStats::PageTotal - 1, 1, .noMagic = true}});
  history.recoverFlash();
  counter.expect("Whole Flash but last no magic", FlashStats::PageTotalSubSect, 0);
  /* Whole flash but last no magic */
  fillPages({{0, FlashStats::PageTotal - 1}, {FlashStats::PageTotal - 1, 1, .noChecksum = true}});
  history.recoverFlash();
  counter.expect("Whole Flash but last no checksum", FlashStats::PageTotalSubSect, 0);
  /* Whole flash but last no magic */
  fillPages({{0, FlashStats::PageTotal - 1}, {FlashStats::PageTotal - 1, 1, .unixOffset = 1}});
  history.recoverFlash();
  counter.expect("Whole Flash but last jump back", FlashStats::PageTotalSubSect, 0);
  /* Ring from half, whole flash */
  fillPages({{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalf}});
  history.recoverFlash();
  counter.expect("Ring from half, whole flash", FlashStats::PageTotal, SectorsFirstHalf);
  /* Ring from half, second half empty page */
  fillPages({{0, PagesFirstHalf}});
  history.recoverFlash();
  counter.expect("Ring from half, second half empty page", PagesFirstHalf, 0);
  /* Ring from half, second half no checksum */
  fillPages({{PagesFirstHalf, PagesSecondHalf, .noChecksum = true}, {0, PagesFirstHalf}});
  history.recoverFlash();
  counter.expect("Ring from half, second half no checksum", PagesFirstHalf, 0);
  /* Ring from half, second half jump back */
  fillPages({{PagesFirstHalf, PagesSecondHalf - 1}, {0, PagesFirstHalf}, {FlashStats::PageTotal - 1, 1, .unixOffset = 1}});
  history.recoverFlash();
  counter.expect("Ring from half, second half jump back", PagesFirstHalf, 0);
  /* Ring from half, second half read failure */
  fillPages({{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalf}});
  {
    auto failure = ReadFailure(PagesFirstHalf + 1);
    history.recoverFlash();
  }
  counter.expect("Ring from half, second half read failure", PagesFirstHalf, 0);
  /* Ring from half, no wraparound at end */
  fillPages({{0, PagesFirstHalf}, {PagesFirstHalf, PagesSecondHalf, .unixOffset = 1}});
  history.recoverFlash();
  counter.expect("Ring from half, no wraparound at end", PagesFirstHalf, 0);
  /* Ring from half, tail no magic */
  fillPages({{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalf - 1}, {PagesFirstHalf - 1, 1, .noMagic = true}});
  history.recoverFlash();
  counter.expect("Ring from half, tail no magic", FlashStats::PageTotalSubSect, SectorsFirstHalf);
  /* Ring from half, tail no checksum */
  fillPages({{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalf - 1}, {PagesFirstHalf - 1, 1, .noChecksum = true}});
  history.recoverFlash();
  counter.expect("Ring from half, tail no checksum", FlashStats::PageTotalSubSect, SectorsFirstHalf);
  /* Ring from half, tail jump back */
  fillPages({{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalf - 1}, {PagesFirstHalf - 1, 1, .unixOffset = 1}});
  history.recoverFlash();
  counter.expect("Ring from half, tail jump back", FlashStats::PageTotalSubSect, SectorsFirstHalf);
  /* Ring from half, tail first page no magic */
  fillPages({{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalfSubSect}, {PagesFirstHalfSubSect, 16, .noMagic = true}});
  history.recoverFlash();
  counter.expect("Ring from half, tail first page no magic", FlashStats::PageTotalSubSect, SectorsFirstHalf);
  /* Ring from half, tail first page no checksum */
  fillPages({{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalfSubSect}, {PagesFirstHalfSubSect, 16, .noChecksum = true}});
  history.recoverFlash();
  counter.expect("Ring from half, tail first page no checksum", FlashStats::PageTotalSubSect, SectorsFirstHalf);
  /* Ring from half, tail first page jump back */
  fillPages({{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalfSubSect}, {PagesFirstHalfSubSect, 16, .unixOffset = 1}});
  history.recoverFlash();
  counter.expect("Ring from half, tail first page jump back (multiple jump back)", PagesFirstHalfSubSect, 0);
  return counter.sum() > 0;
}
