#include <algorithm>
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

static uint8_t buffer[FlashStats::AddrEnd];
static void *const bufferSlices = buffer + FlashStats::AddrStart;
static SensorSlice *const slicesAll = reinterpret_cast<SensorSlice *>(bufferSlices);

static std::vector<uint16_t> NoBrokenBufferPages = {};
static std::vector<uint16_t> &brokenBufferPages = NoBrokenBufferPages;

SpiFlashOpResult spi_flash_read(size_t const offset, void *const target, size_t const size) {
  if (!brokenBufferPages.empty()) {
    uint16_t const pageID = (offset - FlashStats::AddrStart) >> FlashStats::PageExp;
    if (std::binary_search(brokenBufferPages.begin(), brokenBufferPages.end(), pageID)) {
      return 1;
    }
  }
  std::memcpy(target, buffer + offset, size);
  return SPI_FLASH_RESULT_OK;
}

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

struct PagePlan {
  uint16_t offset, count;
  bool noMagic = false;
  bool noChecksum = false;
  bool badFirstTimestamp = false;
  bool badRecordOrder = false;
  uint64_t unixOffset = 0; /* When not zero, forcing this (and later to use this offset) */
};

struct Tester {
  static uint32_t const bufferSlicesSize = FlashStats::AddrEnd - FlashStats::AddrStart;

  size_t pass = 0;
  size_t total = 0;

  void run(char const *const title, uint16_t const count, uint16_t const head, std::vector<PagePlan> const &plans, std::vector<uint16_t> brokenPages = {}) {
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
    std::sort(brokenPages.begin(), brokenPages.end());
    brokenBufferPages = brokenPages;
    std::printf("%zu broken pages\n", brokenBufferPages.size());
    history.recoverFlash();
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

COMPCONST uint16_t const SectorsFirstHalf = FlashStats::SectTotal / 2;
COMPCONST uint16_t const PagesFirstHalf = SectorsFirstHalf << FlashStats::SectPageFactor;
COMPCONST uint16_t const PagesFirstHalfSubSect = PagesFirstHalf - FlashStats::SectPageCount;
COMPCONST uint16_t const PagesSecondHalf = FlashStats::PageTotal - PagesFirstHalf;

int main() {
  Tester tester = {};
  tester.run("Empty Flash", 0, 0, {});
  tester.run("First page", 1, 0, {{0, 1}});
  tester.run("First 15 pages", 15, 0, {{0, 15}});
  tester.run("First 16 pages", 16, 0, {{0, 16}});
  tester.run("First 17 pages", 17, 0, {{0, 17}});
  tester.run("No magic at second sector head", 16, 0, {{0, 16}, {16, 1, .noMagic = true}});
  tester.run("No checksum at second sector head", 16, 0, {{0, 16}, {16, 1, .noChecksum = true}});
  tester.run("Jump back at second sector head", 16, 0, {{0, 16}, {16, 1, .unixOffset = 1}});
  tester.run("Read failure at second sector head", 16, 0, {{0, 32}}, {16});
  tester.run("Non-empty page after empty page in same sector", 16, 0, {{0, 16}, {17, 1}});
  tester.run("Invalid timestamp at second sector head", 16, 0, {{0, 16}, {16, 1, .badFirstTimestamp = true}});
  tester.run("Record timestamp jump back in second sector head", 16, 0, {{0, 16}, {16, 1, .badRecordOrder = true}});
  tester.run("Jump back at second page in second sector", 16, 0, {{0, 18}, {17, 1, .unixOffset = 1}});
  tester.run("Whole Flash", FlashStats::PageTotal, 0, {{0, FlashStats::PageTotal}});
  tester.run("Whole Flash but last no magic", FlashStats::PageTotalSubSect, 0, {{0, FlashStats::PageTotal - 1}, {FlashStats::PageTotal - 1, 1, .noMagic = true}});
  tester.run("Whole Flash but last no checksum", FlashStats::PageTotalSubSect, 0, {{0, FlashStats::PageTotal - 1}, {FlashStats::PageTotal - 1, 1, .noChecksum = true}});
  tester.run("Whole Flash but last jump back", FlashStats::PageTotalSubSect, 0, {{0, FlashStats::PageTotal - 1}, {FlashStats::PageTotal - 1, 1, .unixOffset = 1}});
  tester.run("Ring from half, whole flash", FlashStats::PageTotal, SectorsFirstHalf, {{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalf}});
  tester.run("Ring from half, second half empty page", PagesFirstHalf, 0, {{0, PagesFirstHalf}});
  tester.run("Ring from half, second half no checksum", PagesFirstHalf, 0, {{PagesFirstHalf, PagesSecondHalf, .noChecksum = true}, {0, PagesFirstHalf}});
  tester.run("Ring from half, second half jump back", PagesFirstHalf, 0, {{PagesFirstHalf, PagesSecondHalf - 1}, {0, PagesFirstHalf}, {FlashStats::PageTotal - 1, 1, .unixOffset = 1}});
  tester.run("Ring from half, second half read failure", PagesFirstHalf, 0, {{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalf}}, {PagesFirstHalf + 1});
  tester.run("Ring from half, no wraparound at end", PagesFirstHalf, 0, {{0, PagesFirstHalf}, {PagesFirstHalf, PagesSecondHalf, .unixOffset = 1}});
  tester.run("Ring from half, tail no magic", FlashStats::PageTotalSubSect, SectorsFirstHalf, {{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalf - 1}, {PagesFirstHalf - 1, 1, .noMagic = true}});
  tester.run("Ring from half, tail no checksum", FlashStats::PageTotalSubSect, SectorsFirstHalf, {{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalf - 1}, {PagesFirstHalf - 1, 1, .noChecksum = true}});
  tester.run("Ring from half, tail jump back", FlashStats::PageTotalSubSect, SectorsFirstHalf, {{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalf - 1}, {PagesFirstHalf - 1, 1, .unixOffset = 1}});
  tester.run("Ring from half, tail first page no magic", FlashStats::PageTotalSubSect, SectorsFirstHalf, {{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalfSubSect}, {PagesFirstHalfSubSect, 16, .noMagic = true}});
  tester.run("Ring from half, tail first page no checksum", FlashStats::PageTotalSubSect, SectorsFirstHalf, {{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalfSubSect}, {PagesFirstHalfSubSect, 16, .noChecksum = true}});
  tester.run("Ring from half, tail first page jump back (multiple jump back)", PagesFirstHalfSubSect, 0, {{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalfSubSect}, {PagesFirstHalfSubSect, 16, .unixOffset = 1}});
  return tester.sum() > 0;
}
