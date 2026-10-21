#pragma once
#include <stdint.h>

// Call under the ESP32 critical section shared by ISR and loop.
// A press is a debounced falling edge, as in the original sketch.
class ButtonEvents {
 public:
  #if defined(ARDUINO_ARCH_ESP32)
  IRAM_ATTR
  #endif
  void edge(uint32_t now) {
    if (seen_ && uint32_t(now - lastEdge_) < 50) return;
    seen_ = true;
    lastEdge_ = now;
    ++count_;
  }
  uint32_t count() const { return count_; }
 private:
  volatile uint32_t count_ = 0, lastEdge_ = 0;
  volatile bool seen_ = false;
};
