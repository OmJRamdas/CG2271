#pragma once
#include <stdint.h>

// Debounced held/released state of the fire button, for hold-to-fire.
// Taps are still counted separately by ButtonEvents (falling edge ISR) so a
// press shorter than the packet interval is not lost. Call update() from loop()
// with the raw pin level (true = pressed, i.e. LOW with INPUT_PULLUP).
class FireHold {
 public:
  static constexpr uint32_t STABLE_MS = 20;

  void update(uint32_t now, bool rawPressed) {
    if (rawPressed != raw_) {
      raw_ = rawPressed;
      changedAt_ = now;
    }
    if (raw_ != held_ && uint32_t(now - changedAt_) >= STABLE_MS) held_ = raw_;
  }
  bool held() const { return held_; }

 private:
  bool raw_ = false, held_ = false;
  uint32_t changedAt_ = 0;
};
