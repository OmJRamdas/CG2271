#pragma once
#include "Wire.h"
extern bool mockPresent;
class Adafruit_I2CDevice {
 public:
  Adafruit_I2CDevice(unsigned, TwoWire *) {}
  bool begin() { return mockPresent; }
  bool detected() { return mockPresent; }
};
