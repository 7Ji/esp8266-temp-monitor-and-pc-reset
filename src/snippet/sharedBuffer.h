#pragma once

#include <cstdint>

static inline constexpr uint16_t SharedBufferSize = 4096;
static uint32_t _sharedBuffer[SharedBufferSize >> 2];
static uint32_t *const sharedWordsBuffer = _sharedBuffer;
static uint8_t *const sharedBytesBuffer = reinterpret_cast<uint8_t *>(sharedWordsBuffer);
static char *const sharedStrBuffer = reinterpret_cast<char *>(sharedBytesBuffer);
