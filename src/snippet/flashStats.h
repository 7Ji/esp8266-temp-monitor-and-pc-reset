#pragma once
#include "compConst.h"
namespace FlashStats {
  COMPCONST uint32_t const AddrStart = 0x100000;
  COMPCONST uint32_t const AddrEnd = 0x3FB000;
  COMPCONST int const SectExp = 12;
  COMPCONST uint16_t const SectSize = 1 << SectExp; /* 4096 */
  COMPCONST uint16_t const SectStart = AddrStart / SectSize;
  COMPCONST uint16_t const SectEnd = AddrEnd / SectSize;
  COMPCONST uint16_t const SectTotal = SectEnd - SectStart;
  COMPCONST int const PageExp = 8;
  COMPCONST int const SectPageFactor = SectExp - PageExp;
  COMPCONST uint8_t const SectPageCount = 1 << SectPageFactor;
  COMPCONST uint16_t const SectWordCount = SectSize / sizeof(uint32_t);
  COMPCONST uint16_t const PageSize = 1 << PageExp; /* 256 */
  COMPCONST uint16_t const PageInSectMask = SectPageCount - 1;
  COMPCONST uint16_t const PageStart = AddrStart / PageSize;
  COMPCONST uint16_t const PageEnd = AddrEnd / PageSize;
  COMPCONST uint16_t const PageTotal = PageEnd - PageStart;
  COMPCONST uint16_t const PageTotalSubSect = PageTotal - SectPageCount;
  COMPCONST uint8_t const PageWordCount = PageSize / sizeof(uint32_t);
}
