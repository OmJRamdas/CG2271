#pragma once
#include "Adafruit_Sensor.h"
#include "Adafruit_I2CDevice.h"
enum { MPU6050_RANGE_2_G, MPU6050_RANGE_250_DEG, MPU6050_BAND_21_HZ };
extern sensors_event_t mockAcceleration, mockGyro;
class Adafruit_MPU6050 {
 public:
  bool begin(unsigned, TwoWire *) { return mockPresent; }
  void setAccelerometerRange(int n) { accel_ = n; }
  void setGyroRange(int n) { gyro_ = n; }
  void setFilterBandwidth(int n) { bandwidth_ = n; }
  void setSampleRateDivisor(int n) { divisor_ = n; }
  int getAccelerometerRange() { return accel_; }
  int getGyroRange() { return gyro_; }
  int getFilterBandwidth() { return bandwidth_; }
  int getSampleRateDivisor() { return divisor_; }
  bool getEvent(sensors_event_t *accel, sensors_event_t *gyro, sensors_event_t *) {
    *accel = mockAcceleration;
    *gyro = mockGyro;
    return mockPresent;
  }
 private:
  int accel_ = 0, gyro_ = 0, bandwidth_ = 0, divisor_ = 0;
};
