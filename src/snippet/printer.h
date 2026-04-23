#pragma once
#include <cinttypes>
#ifdef PRINTER_SERIAL
#define PRINTF Serial.printf
#define PRINTLN Serial.println
#else
#include <cstdio>
#define PRINTF std::printf
#define PRINTLN std::puts
#endif
