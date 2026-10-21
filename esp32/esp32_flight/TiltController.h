#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_I2CDevice.h>
#include <math.h>

// Adafruit edition: this module owns the MPU-6050, not Wi-Fi, Serial, UART, or the other sensors.
// Call begin() once after Wire.begin(), then call update() frequently in loop().
// All calls must come from ONE task. If multiple tasks share Wire, protect it
// with the team's I2C mutex, including calls made by other devices.

/*
 * READING GUIDE
 * Read update() first: its six steps describe the entire processing sequence.
 * Then find each helper below when you want to understand that step's details.
 * The class remembers previous angles, gyro offsets, and neutral orientation.
 * Names ending with '_' are values stored between function calls.
 */

struct TiltConfig {
  float pitchSign = 1.0f;       // Change to -1 if the game's direction is reversed.
  float rollSign = 1.0f;
  float yawSign = 1.0f;
};

struct TiltReading {
  float pitchDeg = 0;          // Tilt relative to the calibrated neutral pose.
  float rollDeg = 0;
  float yawRateDps = 0;        // Rotation SPEED about sensor Z, not heading.
  float pitch = 0;             // Game controls: -1 .. +1.
  float roll = 0;
  float yaw = 0;
  uint32_t sampleTimeMs = 0;
  bool valid = false;
};

enum class TiltStatus { NotStarted, Calibrating, Ready, Fault, Stale };

class TiltController {
 public:
  // Named constants keep the timing, units, and game mapping visible.
  static constexpr uint32_t SAMPLE_INTERVAL_US = 10000;  // 100 Hz sampling.
  static constexpr uint16_t CALIBRATION_SAMPLES = 200;  // About 2 seconds.
  static constexpr uint32_t STALE_AFTER_MS = 150;
  static constexpr float MAX_PITCH_DEG = 45.0f;
  static constexpr float MAX_ROLL_DEG = 45.0f;
  static constexpr float MAX_YAW_RATE_DPS = 90.0f;

  explicit TiltController(const TiltConfig &config = TiltConfig())
      : config_(config) {}

  // Adafruit handles register access and unit scaling. Startup is synchronous:
  // its begin() performs reset/wake delays; calibration is scheduled by update().
  bool begin(TwoWire &bus = Wire, uint8_t address = 0x68) {
    started_ = false;
    reading_ = TiltReading();
    state_ = TiltStatus::Fault;
    presenceCheck_ = Adafruit_I2CDevice(address, &bus);
    if (!sensor_.begin(address, &bus) || !presenceCheck_.begin()) return false;
    sensor_.setAccelerometerRange(MPU6050_RANGE_2_G);
    sensor_.setGyroRange(MPU6050_RANGE_250_DEG);
    sensor_.setFilterBandwidth(MPU6050_BAND_21_HZ);
    sensor_.setSampleRateDivisor(9);  // 1 kHz / (1 + 9) = 100 Hz.
    if (sensor_.getAccelerometerRange() != MPU6050_RANGE_2_G ||
        sensor_.getGyroRange() != MPU6050_RANGE_250_DEG ||
        sensor_.getFilterBandwidth() != MPU6050_BAND_21_HZ ||
        sensor_.getSampleRateDivisor() != 9) return false;
    started_ = true;
    startCalibration();
    return true;
  }

  // Hold still, component side up, in a comfortable near-level neutral pose.
  // This measures gyro bias AND records the pose that should mean no input.
  void startCalibration() {
    if (!started_) return;
    state_ = TiltStatus::Calibrating;
    reading_ = TiltReading();
    filterInitialized_ = false;
    clearCalibrationSamples();
    calibrationStartMs_ = millis();
    lastPollUs_ = micros();
  }

  // Recenter immediately; unlike calibration, this does not remeasure gyro bias.
  bool recenter() {
    if (!latest().valid) return false;
    neutralPitchDeg_ = filteredPitchDeg_;
    neutralRollDeg_ = filteredRollDeg_;
    publishReading(reading_.yawRateDps);
    return true;
  }

  // Read this function first. The helper functions below explain each step.
  // Call frequently from loop(); true means a fresh usable reading was produced.
  bool update() {
    if (!started_) return false;

    // 1. Wait until the next 10 ms sample time, without a delay loop.
    const uint32_t sampleTimeUs = micros();
    const uint32_t elapsedSincePollUs = sampleTimeUs - lastPollUs_;
    if (elapsedSincePollUs < SAMPLE_INTERVAL_US) return false;
    lastPollUs_ = sampleTimeUs;  // Skip missed samples rather than catching up.

    // 2. Get acceleration and rotation speed from Adafruit.
    Measurements sample;
    if (!readMeasurements(sample)) {
      fail();
      return false;
    }

    // 3. Estimate pitch and roll from gravity's X/Y/Z components.
    if (!hasUsableGravityReference(sample)) return false;
    const Angles accelerationAngles = calculateAccelerometerAngles(sample);

    // 4. At startup, learn gyro bias and record the neutral pose.
    if (state_ == TiltStatus::Calibrating) {
      return collectCalibrationSample(sample, accelerationAngles, sampleTimeUs);
    }

    // 5. Use gyro motion for quick changes and gravity to correct drift.
    if (!updateFilteredAngles(sample, accelerationAngles, sampleTimeUs)) return false;

    // 6. Convert neutral-relative angles into the game's -1 to +1 controls.
    const float correctedYawRateDps = sample.gyroZDps - gyroZOffsetDps_;
    publishReading(correctedYawRateDps);
    return true;
  }

  // Always fetch a fresh snapshot before sending. If samples are stale,
  // uncalibrated, or faulty, the game controls are zero and valid is false.
  TiltReading latest() const {
    TiltReading out = reading_;
    out.valid = state_ == TiltStatus::Ready && out.valid &&
        uint32_t(millis() - out.sampleTimeMs) <= STALE_AFTER_MS;
    if (!out.valid) {
      out.pitch = 0;
      out.roll = 0;
      out.yaw = 0;
    }
    return out;
  }

  TiltStatus status() const {
    if (state_ == TiltStatus::Ready && !latest().valid) return TiltStatus::Stale;
    return state_;
  }
  uint16_t calibrationSamples() const { return calibrationCount_; }

 private:
  // Internal values have units in their names. Adafruit supplies m/s^2 and
  // rad/s; this module uses g, degrees, and degrees/second for its calculations.
  struct Measurements {
    float accelerationXG = 0;
    float accelerationYG = 0;
    float accelerationZG = 0;
    float gyroXDps = 0;
    float gyroYDps = 0;
    float gyroZDps = 0;
    float accelerationMagnitudeG = 0;
  };
  struct Angles {
    float pitchDeg = 0;
    float rollDeg = 0;
  };

  static constexpr float STANDARD_GRAVITY = 9.80665f;
  static constexpr float RADIANS_TO_DEGREES = 57.2957795131f;
  static constexpr float DEGREES_TO_RADIANS = 0.01745329252f;
  static constexpr float FILTER_TIME_CONSTANT_SECONDS = 0.25f;
  static constexpr uint32_t SENSOR_SETTLE_MS = 250;
  static constexpr float CALIBRATION_GRAVITY_TOLERANCE_G = 0.10f;
  static constexpr float CALIBRATION_MAX_ROTATION_DPS = 5.0f;
  static constexpr float CALIBRATION_MAX_TILT_DEG = 45.0f;
  static constexpr float CALIBRATION_MOVEMENT_G = 0.03f;
  static constexpr float TILT_DEADZONE_DEG = 1.5f;
  static constexpr float YAW_DEADZONE_DPS = 3.0f;

  // STEP 2: sensor reading and unit conversion.
  bool readMeasurements(Measurements &result) {
    // ACK checking detects common disconnects, but does not verify every byte
    // of the following read. Adafruit's getEvent() hides some read errors.
    // The README explains this limitation; valid is not a transport guarantee.
    if (!presenceCheck_.detected()) return false;

    sensors_event_t accelerationEvent = {};
    sensors_event_t gyroEvent = {};
    sensors_event_t temperatureEvent = {};  // Required by the API; unused here.
    if (!sensor_.getEvent(&accelerationEvent, &gyroEvent, &temperatureEvent)) return false;

    // Convert m/s^2 to g. Do not apply raw register scaling a second time.
    result.accelerationXG = accelerationEvent.acceleration.x / STANDARD_GRAVITY;
    result.accelerationYG = accelerationEvent.acceleration.y / STANDARD_GRAVITY;
    result.accelerationZG = accelerationEvent.acceleration.z / STANDARD_GRAVITY;

    // Convert radians/second to degrees/second.
    result.gyroXDps = gyroEvent.gyro.x * RADIANS_TO_DEGREES;
    result.gyroYDps = gyroEvent.gyro.y * RADIANS_TO_DEGREES;
    result.gyroZDps = gyroEvent.gyro.z * RADIANS_TO_DEGREES;

    // Reject non-finite values and values outside the configured ranges.
    // Small margins allow rounding around +/-2 g and +/-250 degrees/second.
    if (!plausible(result.accelerationXG, 2.1f) ||
        !plausible(result.accelerationYG, 2.1f) ||
        !plausible(result.accelerationZG, 2.1f) ||
        !plausible(result.gyroXDps, 260.0f) ||
        !plausible(result.gyroYDps, 260.0f) ||
        !plausible(result.gyroZDps, 260.0f)) return false;

    const float x = result.accelerationXG;
    const float y = result.accelerationYG;
    const float z = result.accelerationZG;
    result.accelerationMagnitudeG = sqrtf(x * x + y * y + z * z);
    return true;
  }

  // STEP 3: gravity-based angles.
  bool hasUsableGravityReference(const Measurements &sample) {
    if (sample.accelerationMagnitudeG >= 0.1f) return true;

    // Near freefall there is no useful gravity reference. Suppress game input
    // until a usable measurement returns; this does not latch a wiring fault.
    reading_.valid = false;
    filterInitialized_ = false;
    if (state_ == TiltStatus::Calibrating) clearCalibrationSamples();
    return false;
  }

  static Angles calculateAccelerometerAngles(const Measurements &sample) {
    const float x = sample.accelerationXG;
    const float y = sample.accelerationYG;
    const float z = sample.accelerationZG;

    // Roll: rotation about X. Pitch: rotation about Y.
    // atan2 gives radians, so multiply to obtain degrees.
    Angles result;
    result.rollDeg = atan2f(y, z) * RADIANS_TO_DEGREES;
    result.pitchDeg = atan2f(-x, sqrtf(y * y + z * z)) * RADIANS_TO_DEGREES;
    return result;
  }

  // STEP 4: collect 200 still samples for gyro bias and the neutral pose.
  bool collectCalibrationSample(const Measurements &sample, const Angles &accelerationAngles,
                                uint32_t sampleTimeUs) {
    // Let the sensor settle before accumulating the calibration window.
    if (uint32_t(millis() - calibrationStartMs_) < SENSOR_SETTLE_MS) return false;
    // Restart the window if the board moves or is far from level. These
    // checks detect obvious motion, not every possible slow movement.
    const bool gravityLooksNormal =
        fabsf(sample.accelerationMagnitudeG - 1.0f) <= CALIBRATION_GRAVITY_TOLERANCE_G;
    const bool rotationIsSmall =
        fabsf(sample.gyroXDps) <= CALIBRATION_MAX_ROTATION_DPS &&
        fabsf(sample.gyroYDps) <= CALIBRATION_MAX_ROTATION_DPS &&
        fabsf(sample.gyroZDps) <= CALIBRATION_MAX_ROTATION_DPS;
    const bool poseIsNearLevel =
        fabsf(accelerationAngles.pitchDeg) <= CALIBRATION_MAX_TILT_DEG &&
        fabsf(accelerationAngles.rollDeg) <= CALIBRATION_MAX_TILT_DEG;
    const bool accelerationChanged =
        fabsf(sample.accelerationXG - firstAccelerationXG_) > CALIBRATION_MOVEMENT_G ||
        fabsf(sample.accelerationYG - firstAccelerationYG_) > CALIBRATION_MOVEMENT_G ||
        fabsf(sample.accelerationZG - firstAccelerationZG_) > CALIBRATION_MOVEMENT_G;
    const bool movedSinceFirstSample = calibrationCount_ > 0 && accelerationChanged;

    if (!gravityLooksNormal || !rotationIsSmall || !poseIsNearLevel || movedSinceFirstSample) {
      clearCalibrationSamples();
      return false;
    }
    if (calibrationCount_ == 0) {
      firstAccelerationXG_ = sample.accelerationXG;
      firstAccelerationYG_ = sample.accelerationYG;
      firstAccelerationZG_ = sample.accelerationZG;
    }
    sumAccelerationXG_ += sample.accelerationXG;
    sumAccelerationYG_ += sample.accelerationYG;
    sumAccelerationZG_ += sample.accelerationZG;
    sumGyroXDps_ += sample.gyroXDps;
    sumGyroYDps_ += sample.gyroYDps;
    sumGyroZDps_ += sample.gyroZDps;
    calibrationCount_++;
    if (calibrationCount_ < CALIBRATION_SAMPLES) return false;

    // A stationary gyro should read zero. Its average reading is the offset
    // that we subtract from later rotation measurements.
    gyroXOffsetDps_ = sumGyroXDps_ / CALIBRATION_SAMPLES;
    gyroYOffsetDps_ = sumGyroYDps_ / CALIBRATION_SAMPLES;
    gyroZOffsetDps_ = sumGyroZDps_ / CALIBRATION_SAMPLES;
    // Ratios are unchanged by averaging, so the same angle helper can use sums.
    Measurements accelerationSum;
    accelerationSum.accelerationXG = sumAccelerationXG_;
    accelerationSum.accelerationYG = sumAccelerationYG_;
    accelerationSum.accelerationZG = sumAccelerationZG_;
    const Angles startingAngles = calculateAccelerometerAngles(accelerationSum);
    filteredRollDeg_ = startingAngles.rollDeg;
    filteredPitchDeg_ = startingAngles.pitchDeg;
    neutralRollDeg_ = filteredRollDeg_;
    neutralPitchDeg_ = filteredPitchDeg_;
    filterInitialized_ = true;
    lastSampleUs_ = sampleTimeUs;
    state_ = TiltStatus::Ready;
    publishReading(sample.gyroZDps - gyroZOffsetDps_);
    return true;
  }

  // STEP 5: complementary filtering.
  bool updateFilteredAngles(const Measurements &sample, const Angles &accelerationAngles,
                            uint32_t sampleTimeUs) {
    const uint32_t elapsedUs = sampleTimeUs - lastSampleUs_;
    const float elapsedSeconds = elapsedUs / 1000000.0f;
    lastSampleUs_ = sampleTimeUs;

    // Magnitude near 1 g suggests that gravity is the main acceleration.
    const bool canTrustGravity = fabsf(sample.accelerationMagnitudeG - 1.0f) <= 0.15f;
    const bool missedTooMuchTime = elapsedSeconds > 0.1f;

    if (!filterInitialized_ || missedTooMuchTime) {
      // We have no reliable gyro history across a large gap. Restart from
      // gravity, but keep the already recorded gyro bias and neutral pose.
      if (!canTrustGravity) {
        filterInitialized_ = false;
        reading_.valid = false;
        return false;
      }
      filteredPitchDeg_ = accelerationAngles.pitchDeg;
      filteredRollDeg_ = accelerationAngles.rollDeg;
      filterInitialized_ = true;
      return true;
    }

    const Angles prediction = predictAnglesFromGyro(sample, elapsedSeconds);

    // gyroWeight near 1 trusts the gyro more. Expressing it using elapsed time
    // keeps the tuning consistent when sample intervals vary slightly.
    float gyroWeight = 1.0f;
    if (canTrustGravity) {
      gyroWeight = FILTER_TIME_CONSTANT_SECONDS /
                   (FILTER_TIME_CONSTANT_SECONDS + elapsedSeconds);
    }
    // If acceleration is far from 1 g, gyroWeight stays 1: skip gravity correction.
    filteredPitchDeg_ = blendAngle(prediction.pitchDeg, accelerationAngles.pitchDeg, gyroWeight);
    filteredRollDeg_ = blendAngle(prediction.rollDeg, accelerationAngles.rollDeg, gyroWeight);
    return true;
  }

  // ADVANCED PART OF STEP 5: rotate the gyro's body-axis rates into angle rates.
  // Near level: gyro X changes roll and gyro Y changes pitch. When tilted,
  // the extra terms account for the axes rotating together.
  // The pitch clamp avoids the singularity at +/-90 degrees. This is a
  // near-level handheld controller, not an inverted-flight attitude estimator.
  Angles predictAnglesFromGyro(const Measurements &sample, float elapsedSeconds) const {
    const float rollRadians = filteredRollDeg_ * DEGREES_TO_RADIANS;
    const float limitedPitchDeg = clamp(filteredPitchDeg_, -80.0f, 80.0f);
    const float pitchRadians = limitedPitchDeg * DEGREES_TO_RADIANS;
    const float sinRoll = sinf(rollRadians);
    const float cosRoll = cosf(rollRadians);
    const float tanPitch = tanf(pitchRadians);

    // A stationary gyro should read zero. Subtract its measured resting offset.
    const float correctedX = sample.gyroXDps - gyroXOffsetDps_;
    const float correctedY = sample.gyroYDps - gyroYOffsetDps_;
    const float correctedZ = sample.gyroZDps - gyroZOffsetDps_;
    const float rollRateDps = correctedX + sinRoll * tanPitch * correctedY
                                         + cosRoll * tanPitch * correctedZ;
    const float pitchRateDps = cosRoll * correctedY - sinRoll * correctedZ;

    // Angle change = rotation speed * elapsed time.
    Angles prediction;
    prediction.rollDeg = wrapDegrees(filteredRollDeg_ + rollRateDps * elapsedSeconds);
    prediction.pitchDeg = wrapDegrees(filteredPitchDeg_ + pitchRateDps * elapsedSeconds);
    return prediction;
  }

  static float blendAngle(float gyroPredictionDeg, float accelerationAngleDeg, float gyroWeight) {
    // Use the shortest angle difference, including across the +/-180 boundary.
    const float gravityCorrectionDeg = wrapDegrees(accelerationAngleDeg - gyroPredictionDeg);
    const float accelerationWeight = 1.0f - gyroWeight;
    return wrapDegrees(gyroPredictionDeg + accelerationWeight * gravityCorrectionDeg);
  }

  // STEP 6: output mapping. Helper functions below also handle angles and state.
  static bool plausible(float value, float maximumMagnitude) {
    return isfinite(value) && fabsf(value) <= maximumMagnitude;
  }

  static float clamp(float x, float low, float high) {
    if (x < low) return low;
    if (x > high) return high;
    return x;
  }
  static float wrapDegrees(float angle) {
    while (angle > 180) angle -= 360;
    while (angle < -180) angle += 360;
    return angle;
  }
  static float gameControl(float value, float fullScale, float deadzone) {
    // A small neutral deadzone reduces unintended motion. Outside it, preserve
    // the repo's direct angle / 45 mapping, clamped to the game's input range.
    if (fabsf(value) <= deadzone) return 0.0f;
    return clamp(value / fullScale, -1.0f, 1.0f);
  }
  void publishReading(float correctedYawRate) {
    reading_.pitchDeg = wrapDegrees(filteredPitchDeg_ - neutralPitchDeg_);
    reading_.rollDeg = wrapDegrees(filteredRollDeg_ - neutralRollDeg_);
    reading_.yawRateDps = correctedYawRate;
    const float pitchControl = gameControl(reading_.pitchDeg, MAX_PITCH_DEG, TILT_DEADZONE_DEG);
    const float rollControl = gameControl(reading_.rollDeg, MAX_ROLL_DEG, TILT_DEADZONE_DEG);
    const float yawControl = gameControl(correctedYawRate, MAX_YAW_RATE_DPS, YAW_DEADZONE_DPS);
    reading_.pitch = config_.pitchSign * pitchControl;
    reading_.roll = config_.rollSign * rollControl;
    reading_.yaw = config_.yawSign * yawControl;
    reading_.sampleTimeMs = millis();
    reading_.valid = true;
  }
  void fail() {
    started_ = false;  // Explicit begin() required after reconnecting hardware.
    state_ = TiltStatus::Fault;
    reading_.valid = false;
  }
  void clearCalibrationSamples() {
    calibrationCount_ = 0;
    sumAccelerationXG_ = 0;
    sumAccelerationYG_ = 0;
    sumAccelerationZG_ = 0;
    sumGyroXDps_ = 0;
    sumGyroYDps_ = 0;
    sumGyroZDps_ = 0;
  }

  // State remembered between calls, grouped by purpose.
  TiltConfig config_;
  TiltReading reading_;
  TiltStatus state_ = TiltStatus::NotStarted;
  Adafruit_MPU6050 sensor_;
  Adafruit_I2CDevice presenceCheck_{0x68, &Wire};
  bool started_ = false;
  bool filterInitialized_ = false;

  uint32_t lastPollUs_ = 0;
  uint32_t lastSampleUs_ = 0;
  uint32_t calibrationStartMs_ = 0;

  uint16_t calibrationCount_ = 0;
  float sumAccelerationXG_ = 0;
  float sumAccelerationYG_ = 0;
  float sumAccelerationZG_ = 0;
  float sumGyroXDps_ = 0;
  float sumGyroYDps_ = 0;
  float sumGyroZDps_ = 0;
  float firstAccelerationXG_ = 0;
  float firstAccelerationYG_ = 0;
  float firstAccelerationZG_ = 0;

  float gyroXOffsetDps_ = 0;
  float gyroYOffsetDps_ = 0;
  float gyroZOffsetDps_ = 0;
  float filteredPitchDeg_ = 0;
  float filteredRollDeg_ = 0;
  float neutralPitchDeg_ = 0;
  float neutralRollDeg_ = 0;
};
