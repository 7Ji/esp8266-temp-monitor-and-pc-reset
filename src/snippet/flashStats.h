#pragma once

#include <cstdint>

namespace FlashStats {
  static inline constexpr uint32_t AddrStart = 0x100000;
  static inline constexpr uint32_t AddrEnd = 0x3FB000;
  static inline constexpr int SectExp = 12;
  static inline constexpr uint16_t SectSize = 1 << SectExp; /* 4096 */
  static inline constexpr uint16_t SectStart = AddrStart / SectSize;
  static inline constexpr uint16_t SectEnd = AddrEnd / SectSize;
  static inline constexpr uint16_t SectTotal = SectEnd - SectStart;
  static inline constexpr int PageExp = 8;
  static inline constexpr int SectPageFactor = SectExp - PageExp;
  static inline constexpr uint8_t SectPageCount = 1 << SectPageFactor;
  static inline constexpr uint16_t SectWordCount = SectSize / sizeof(uint32_t);
  static inline constexpr uint16_t PageSize = 1 << PageExp; /* 256 */
  static inline constexpr uint16_t PageInSectMask = SectPageCount - 1;
  static inline constexpr uint16_t PageStart = AddrStart / PageSize;
  static inline constexpr uint16_t PageEnd = AddrEnd / PageSize;
  static inline constexpr uint16_t PageTotal = PageEnd - PageStart;
  static inline constexpr uint16_t PageTotalSubSect = PageTotal - SectPageCount;
  static inline constexpr uint8_t PageWordCount = PageSize / sizeof(uint32_t);
}
