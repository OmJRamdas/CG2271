#pragma once
#include <stdint.h>
extern uint64_t mockTimeUs;
inline uint32_t micros() { return static_cast<uint32_t>(mockTimeUs); }
inline uint32_t millis() { return static_cast<uint32_t>(mockTimeUs / 1000); }
