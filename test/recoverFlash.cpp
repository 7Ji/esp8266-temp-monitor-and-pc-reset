#include <algorithm>
#include <vector>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <unistd.h>

#include "snippet/flashStats.h"
#include "snippet/sharedBuffer.h"

typedef int SpiFlashOpResult;
static inline constexpr SpiFlashOpResult SPI_FLASH_RESULT_OK = 0;

namespace Printer {
  using std::printf;
  static constexpr auto println = std::puts;
};

#include "snippet/sensorPage.h"
uint32_t SensorPage::actualChecksum() const {
  size_t length = sizeof(unixOffset) + sizeof(records);
  uint32_t crc = 0xffffffff;

  uint8_t const* ldata = reinterpret_cast<uint8_t const*>(&unixOffset);
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

static uint8_t buffer[FlashStats::AddrEnd];
static void *const bufferPages = buffer + FlashStats::AddrStart;
static SensorPage *const pagesAll = reinterpret_cast<SensorPage *>(bufferPages);

static std::vector<uint16_t> NoBrokenBufferSectors = {};
static std::vector<uint16_t> &brokenBufferSectors = NoBrokenBufferSectors;

SpiFlashOpResult spi_flash_read(size_t const offset, void *const target, size_t const size) {
  if (!brokenBufferSectors.empty()) {
    if (std::binary_search(brokenBufferSectors.begin(), brokenBufferSectors.end(), (offset - FlashStats::AddrStart) >> FlashStats::SectExp)) {
      return 1;
    }
  }
  std::memcpy(target, buffer + offset, size);
  return SPI_FLASH_RESULT_OK;
}

#include "snippet/recoverFlash.h"
[[gnu::always_inline]] inline bool RecoverFlash::readFlashSector(uint16_t const flashSectorID) {
  if (!brokenBufferSectors.empty()) {
    if (std::binary_search(brokenBufferSectors.begin(), brokenBufferSectors.end(), flashSectorID - FlashStats::SectStart)) {
      std::printf("Failed to read flash sector f%" PRIu16 " when recovering flash\n", flashSectorID);
      return false;
    }
  }
  std::memcpy(sharedBytesBuffer, buffer + (flashSectorID << FlashStats::SectExp), FlashStats::SectSize);
  return true;
}

struct PagePlan {
  uint16_t offset, count;
  bool noMagic = false;
  bool noChecksum = false;
  bool nonZeroTimestamp = false;
  bool lastRecordNotIncreasing = false;
  uint64_t unixOffset = 0; /* When not zero, forcing this (and later to use this offset) */
};

struct Tester {
  static uint32_t const bufferPagesSize = FlashStats::AddrEnd - FlashStats::AddrStart;

  size_t pass = 0;
  size_t total = 0;

  void run(char const *const title, uint16_t const head, uint16_t const count, std::vector<PagePlan> const &plans, std::vector<uint16_t> brokenSectors = {}) {
    uint16_t recoveredCountL2 = 0; /* page */
    uint16_t recoveredHeadL2 = 0; /* sector */

    memset(bufferPages, 0xFF, bufferPagesSize);

    uint64_t unixOffset = 1;
    for (PagePlan const &plan: plans) {
      SensorPage *const pages = pagesAll + plan.offset;
      if (plan.unixOffset) {
        unixOffset = plan.unixOffset;
      }
      for (auto i = 0; i < plan.count; ++i) {
        SensorPage *page = pages + i;
        page->magic = plan.noMagic ? 0 : page->Magic;
        page->unixOffset = unixOffset;
        memset(page->records, 9, sizeof(page->records)); /* temp 9.9, humid 9.9 */
        for (auto j = 0; j < SensorPage::MaxRecords; ++j) {
          page->records[j].timestamp = j;
        }
        if (plan.nonZeroTimestamp) {
          page->records[0].timestamp = 1;
        }
        if (plan.lastRecordNotIncreasing) {
          page->records[SensorPage::MaxRecordsSub1].timestamp = page->records[SensorPage::MaxRecordsSub1 - 1].timestamp;
        }
        page->checksum = plan.noChecksum ? 0 : page->actualChecksum();
        unixOffset += 3600;
      }
    }
    std::sort(brokenSectors.begin(), brokenSectors.end());
    brokenBufferSectors = brokenSectors;
    std::printf("%zu broken pages\n", brokenBufferSectors.size());
    RecoverFlash::recoverFlash(recoveredHeadL2, recoveredCountL2);
    bool const cond = count == recoveredCountL2 && head == recoveredHeadL2;
    pass += cond;
    ++total;
    std::printf("%s%03zu: %s: %s\n", cond ? "\033[32m" : "\033[31m", total, title, cond ? "PASS\033[0m" : "FAIL");
    if (!cond) {
      std::printf(" - countL2 expected %hu got %hu, headL2 expected %hu got %hu\033[0m\n", count, recoveredCountL2, head, recoveredHeadL2);
    }
  }

  size_t sum() const {
    size_t const fail = total - pass;
    std::printf("Total: %zu, Pass: %zu, Fail: %zu\n", total, pass, fail);
    return fail;
  }
};

static inline constexpr uint16_t SectorsFirstHalf = FlashStats::SectTotal / 2;
static inline constexpr uint16_t PagesFirstHalf = SectorsFirstHalf << FlashStats::SectPageFactor;
static inline constexpr uint16_t PagesFirstHalfSubSect = PagesFirstHalf - FlashStats::SectPageCount;
static inline constexpr uint16_t PagesSecondHalf = FlashStats::PageTotal - PagesFirstHalf;
static inline constexpr uint16_t SectorsMiddle = FlashStats::SectTotal / 3;
static inline constexpr uint16_t PagesMiddle = SectorsMiddle << FlashStats::SectPageFactor;
static inline constexpr uint16_t SectorsMiddleLater = SectorsMiddle + 4;
static inline constexpr uint16_t PagesMiddleLater = SectorsMiddleLater << FlashStats::SectPageFactor;

int main() {
  Tester tester = {};
  tester.run("Empty Flash", 0, 0, {});
  tester.run("First page", 0, 1, {{0, 1}});
  tester.run("First 15 pages", 0, 15, {{0, 15}});
  tester.run("First 16 pages", 0, 16, {{0, 16}});
  tester.run("First 17 pages", 0, 17, {{0, 17}});
  tester.run("No magic at second sector head", 0, 16, {{0, 16}, {16, 1, .noMagic = true}});
  tester.run("No checksum at second sector head", 0, 16, {{0, 16}, {16, 1, .noChecksum = true}});
  tester.run("Jump back at second sector head", 1, 17, {{0, 16}, {16, 17, .unixOffset = 1}});
  tester.run("Read failure at second sector", 0, 16, {{0, 32}}, {1});
  tester.run("Non-empty page after empty page in same sector", 0, 16, {{0, 16}, {17, 1}});
  tester.run("Invalid timestamp at second sector head", 0, 16, {{0, 16}, {16, 1, .nonZeroTimestamp = true}});
  tester.run("Last record timestamp not increasing in second sector head", 0, 16, {{0, 16}, {16, 1, .lastRecordNotIncreasing = true}});
  tester.run("Jump back at second page in second sector", 0, 16, {{0, 18}, {17, 1, .unixOffset = 1}});
  tester.run("Middle orphan page", SectorsMiddle, 1, {{PagesMiddle, 1}});
  tester.run("Middle orphan partial sector", SectorsMiddle, 15, {{PagesMiddle, 15}});
  tester.run("Middle orphan spanning sectors", SectorsMiddle, 17, {{PagesMiddle, 17}});
  tester.run("Middle orphan beats older head chunk", SectorsMiddle, 17, {{0, 16}, {PagesMiddle, 17}});
  tester.run("Newer head chunk beats older middle orphan", 0, 16, {{PagesMiddle, 17}, {0, 16}});
  tester.run("Newer middle orphan beats older middle orphan", SectorsMiddleLater, 16, {{PagesMiddle, 17}, {PagesMiddleLater, 16}});
  tester.run("Whole Flash", 0, FlashStats::PageTotal, {{0, FlashStats::PageTotal}});
  tester.run("Whole Flash but last no magic", 0, FlashStats::PageTotalSubSect, {{0, FlashStats::PageTotal - 1}, {FlashStats::PageTotal - 1, 1, .noMagic = true}});
  tester.run("Whole Flash but last no checksum", 0, FlashStats::PageTotalSubSect, {{0, FlashStats::PageTotal - 1}, {FlashStats::PageTotal - 1, 1, .noChecksum = true}});
  tester.run("Whole Flash but last jump back", 0, FlashStats::PageTotalSubSect, {{0, FlashStats::PageTotal - 1}, {FlashStats::PageTotal - 1, 1, .unixOffset = 1}});
  tester.run("Ring from half, whole flash", SectorsFirstHalf, FlashStats::PageTotal, {{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalf}});
  tester.run("Ring from half, second half empty", SectorsFirstHalf, PagesSecondHalf, {{PagesFirstHalf, PagesSecondHalf}});
  tester.run("Ring from half, second half no checksum", 0, PagesFirstHalf, {{PagesFirstHalf, PagesSecondHalf, .noChecksum = true}, {0, PagesFirstHalf}});
  tester.run("Ring from half, second half jump back", 0, PagesFirstHalf, {{PagesFirstHalf, PagesSecondHalf - 1}, {0, PagesFirstHalf}, {FlashStats::PageTotal - 1, 1, .unixOffset = 1}});
  tester.run("Ring from half, second half read failure", SectorsFirstHalf, FlashStats::PageTotalSubSect, {{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalf}}, {SectorsFirstHalf - 1});
  tester.run("Ring from half, no wraparound at end", PagesFirstHalf > PagesSecondHalf ? 0 : SectorsFirstHalf, std::max(PagesFirstHalf, PagesSecondHalf), {{0, PagesFirstHalf}, {PagesFirstHalf, PagesSecondHalf, .unixOffset = 1}});
  tester.run("Ring from half, tail no magic", SectorsFirstHalf, FlashStats::PageTotalSubSect, {{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalf - 1}, {PagesFirstHalf - 1, 1, .noMagic = true}});
  tester.run("Ring from half, tail no checksum", SectorsFirstHalf, FlashStats::PageTotalSubSect, {{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalf - 1}, {PagesFirstHalf - 1, 1, .noChecksum = true}});
  tester.run("Ring from half, tail jump back", SectorsFirstHalf, FlashStats::PageTotalSubSect, {{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalf - 1}, {PagesFirstHalf - 1, 1, .unixOffset = 1}});
  tester.run("Ring from half, tail first page no magic", SectorsFirstHalf, FlashStats::PageTotalSubSect, {{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalfSubSect}, {PagesFirstHalfSubSect, 16, .noMagic = true}});
  tester.run("Ring from half, tail first page no checksum", SectorsFirstHalf, FlashStats::PageTotalSubSect, {{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalfSubSect}, {PagesFirstHalfSubSect, 16, .noChecksum = true}});
  tester.run("Ring from half, tail first page jump back (multiple jump back)", SectorsFirstHalf, FlashStats::PageTotalSubSect, {{PagesFirstHalf, PagesSecondHalf}, {0, PagesFirstHalfSubSect}, {PagesFirstHalfSubSect, 16, .unixOffset = 1}});
  return tester.sum() > 0;
}
