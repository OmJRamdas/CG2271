#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>

// Minimal register-level MPU-6050 driver with the subset of the
// Adafruit_MPU6050 API that TiltController uses.
//
// Why: Adafruit_MPU6050::begin() rejects any chip whose WHO_AM_I is not 0x68.
// Many GY-521 boards carry register-compatible clones (MPU-6500 family) that
// report 0x70/0x71/0x72/0x73 or 0x98, so the library refuses them and tilt
// stays in Fault. This driver accepts those IDs and reads the same registers.

// Same names and register values as Adafruit_MPU6050.h, so TiltController is unchanged.
enum { MPU6050_RANGE_2_G = 0 };       // ACCEL_CONFIG AFS_SEL
enum { MPU6050_RANGE_250_DEG = 0 };   // GYRO_CONFIG  FS_SEL
enum { MPU6050_BAND_21_HZ = 4 };      // CONFIG       DLPF_CFG

class MpuRegisters {
 public:
  bool begin(uint8_t address, TwoWire *bus) {
    address_ = address;
    bus_ = bus;
    if (!readRegisters(REG_WHO_AM_I, &whoAmI_, 1) || !knownId(whoAmI_)) return false;
    if (!write(REG_PWR_MGMT_1, 0x80)) return false;        // device reset
    delay(100);
    if (!write(REG_PWR_MGMT_1, 0x01)) return false;        // wake, PLL with gyro X clock
    delay(10);
    return true;
  }

  uint8_t whoAmI() const { return whoAmI_; }

  void setAccelerometerRange(int range) { updateBits(REG_ACCEL_CONFIG, 0x18, uint8_t(range << 3)); }
  void setGyroRange(int range) { updateBits(REG_GYRO_CONFIG, 0x18, uint8_t(range << 3)); }
  void setFilterBandwidth(int band) {
    updateBits(REG_CONFIG, 0x07, uint8_t(band));
    // MPU-6500-family clones filter the accelerometer separately (ACCEL_CONFIG2).
    if (whoAmI_ != 0x68) updateBits(REG_ACCEL_CONFIG2, 0x07, uint8_t(band));
  }
  void setSampleRateDivisor(int divisor) { write(REG_SMPLRT_DIV, uint8_t(divisor)); }

  int getAccelerometerRange() { return (readByte(REG_ACCEL_CONFIG) >> 3) & 3; }
  int getGyroRange() { return (readByte(REG_GYRO_CONFIG) >> 3) & 3; }
  int getFilterBandwidth() { return readByte(REG_CONFIG) & 7; }
  int getSampleRateDivisor() { return readByte(REG_SMPLRT_DIV); }

  // Fills acceleration in m/s^2 and gyro in rad/s, like Adafruit's getEvent().
  // Assumes the +/-2 g and +/-250 deg/s ranges TiltController configures.
  bool getEvent(sensors_event_t *accel, sensors_event_t *gyro, sensors_event_t *temp) {
    uint8_t b[14];
    if (!readRegisters(REG_ACCEL_XOUT_H, b, sizeof b)) return false;
    const float g = 9.80665f / 16384.0f;                    // m/s^2 per LSB at +/-2 g
    const float rad = (3.14159265f / 180.0f) / 131.0f;      // rad/s per LSB at +/-250 deg/s
    *accel = sensors_event_t();
    *gyro = sensors_event_t();
    accel->acceleration.x = be16(b, 0) * g;
    accel->acceleration.y = be16(b, 2) * g;
    accel->acceleration.z = be16(b, 4) * g;
    gyro->gyro.x = be16(b, 8) * rad;
    gyro->gyro.y = be16(b, 10) * rad;
    gyro->gyro.z = be16(b, 12) * rad;
    if (temp) *temp = sensors_event_t();
    return true;
  }

 private:
  static constexpr uint8_t REG_SMPLRT_DIV = 0x19;
  static constexpr uint8_t REG_CONFIG = 0x1A;
  static constexpr uint8_t REG_GYRO_CONFIG = 0x1B;
  static constexpr uint8_t REG_ACCEL_CONFIG = 0x1C;
  static constexpr uint8_t REG_ACCEL_CONFIG2 = 0x1D;
  static constexpr uint8_t REG_ACCEL_XOUT_H = 0x3B;
  static constexpr uint8_t REG_PWR_MGMT_1 = 0x6B;
  static constexpr uint8_t REG_WHO_AM_I = 0x75;

  static bool knownId(uint8_t id) {
    // 0x68 genuine MPU-6050; 0x70/0x71/0x73 MPU-6500/9250/9255; 0x72 and 0x98 common clones.
    return id == 0x68 || id == 0x70 || id == 0x71 || id == 0x72 || id == 0x73 || id == 0x98;
  }
  static int16_t be16(const uint8_t *b, int i) { return int16_t((b[i] << 8) | b[i + 1]); }

  bool write(uint8_t reg, uint8_t value) {
    bus_->beginTransmission(address_);
    bus_->write(reg);
    bus_->write(value);
    return bus_->endTransmission() == 0;
  }
  bool readRegisters(uint8_t reg, uint8_t *out, uint8_t len) {
    bus_->beginTransmission(address_);
    bus_->write(reg);
    if (bus_->endTransmission(false) != 0) return false;
    if (bus_->requestFrom(address_, len) != len) return false;
    for (uint8_t i = 0; i < len; ++i) out[i] = uint8_t(bus_->read());
    return true;
  }
  uint8_t readByte(uint8_t reg) {
    uint8_t v = 0xFF;                       // 0xFF makes TiltController's checks fail on error
    readRegisters(reg, &v, 1);
    return v;
  }
  void updateBits(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t v = 0;
    if (readRegisters(reg, &v, 1)) write(reg, uint8_t((v & ~mask) | (value & mask)));
  }

  uint8_t address_ = 0x68;
  TwoWire *bus_ = &Wire;
  uint8_t whoAmI_ = 0;
};
