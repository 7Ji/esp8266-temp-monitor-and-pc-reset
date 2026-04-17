#ifndef COMPCONST
#define COMPCONST static inline constexpr
#endif
  COMPCONST uint32_t const FlashAddrStart = 0x100000;
  COMPCONST uint32_t const FlashAddrEnd = 0x3FB000;
  COMPCONST int const FlashSectExp = 12;
  COMPCONST uint16_t const FlashSectSize = 1 << FlashSectExp; /* 4096 */
  COMPCONST uint16_t const FlashSectStart = FlashAddrStart / FlashSectSize;
  COMPCONST uint16_t const FlashSectEnd = FlashAddrEnd / FlashSectSize;
  COMPCONST uint16_t const FlashSectTotal = FlashSectEnd - FlashSectStart;
  COMPCONST int const FlashPageExp = 8;
  COMPCONST int const FlashSectPageFactor = FlashSectExp - FlashPageExp;
  COMPCONST uint8_t const FlashSectPageCount = 1 << FlashSectPageFactor;
  COMPCONST uint16_t const FlashSectWordCount = FlashSectSize / sizeof(uint32_t);
  COMPCONST uint16_t const FlashPageSize = 1 << FlashPageExp; /* 256 */
  COMPCONST uint16_t const FlashPageInSectMask = FlashSectPageCount - 1;
  COMPCONST uint16_t const FlashPageStart = FlashAddrStart / FlashPageSize;
  COMPCONST uint16_t const FlashPageEnd = FlashAddrEnd / FlashPageSize;
  COMPCONST uint16_t const FlashPageTotal = FlashPageEnd - FlashPageStart;
  COMPCONST uint16_t const FlashPageTotalSubSect = FlashPageTotal - FlashSectPageCount;
  COMPCONST uint8_t const FlashPageWordCount = FlashPageSize / sizeof(uint32_t);
