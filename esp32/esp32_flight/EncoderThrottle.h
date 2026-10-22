#pragma once
#include <stdint.h>

// Quadrature decoder that turns encoder rotation into a 0..100 throttle level.
// Hardware independent: feed it the 2-bit pin state ((CLK << 1) | DT) from a
// CHANGE interrupt on either pin. Call update() and read level() under the
// same critical section (ISR vs loop), as ButtonEvents does.
struct EncoderConfig {
  int step = 5;                // throttle percent per detent
  int transitionsPerStep = 4;  // quadrature transitions per detent; verify on the real knob
  int initial = 50;
  bool reverse = false;
};

class EncoderThrottle {
 public:
  explicit EncoderThrottle(const EncoderConfig &config = EncoderConfig())
      : config_(config), level_(clamp(config.initial)) {}

  // Latch the idle pin state before enabling interrupts.
  void begin(uint8_t state) {
    previous_ = state & 3;
    accumulated_ = 0;
  }

  #if defined(ARDUINO_ARCH_ESP32)
  IRAM_ATTR
  #endif
  void update(uint8_t state) {
    state &= 3;
    if (state == previous_) return;  // Contact bounce back to the same state.
    if ((state ^ previous_) == 3) {  // Both bits changed: a transition was missed.
      accumulated_ = 0;
      previous_ = state;
      ++skipped_;
      return;
    }
    static const int8_t table[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
    int movement = table[(previous_ << 2) | state];
    previous_ = state;
    if (config_.reverse) movement = -movement;
    accumulated_ += movement;
    if (accumulated_ >= config_.transitionsPerStep) {
      accumulated_ = 0;
      level_ = clamp(level_ + config_.step);
    } else if (accumulated_ <= -config_.transitionsPerStep) {
      accumulated_ = 0;
      level_ = clamp(level_ - config_.step);
    }
  }

  int level() const { return level_; }
  uint32_t skipped() const { return skipped_; }

 private:
  static int clamp(int value) { return value < 0 ? 0 : (value > 100 ? 100 : value); }
  EncoderConfig config_;
  volatile int level_;
  volatile int accumulated_ = 0;
  volatile uint32_t skipped_ = 0;
  volatile uint8_t previous_ = 0;
};
