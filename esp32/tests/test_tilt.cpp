#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <initializer_list>
#include "TiltController.h"
#include "ButtonEvents.h"

uint64_t mockTimeUs = 0;
TwoWire Wire;
bool mockPresent = true;
sensors_event_t mockAcceleration, mockGyro;

static void tick(TiltController &tilt, unsigned samples = 1) {
  for (unsigned i = 0; i < samples; ++i) {
    mockTimeUs += 10000;
    tilt.update();
  }
}
static void level() {
  mockAcceleration = sensors_event_t();
  mockAcceleration.acceleration.z = 9.80665f;
  mockGyro = sensors_event_t();
}
static void near(float actual, float expected, float tolerance = 0.015f) {
  assert(fabsf(actual - expected) < tolerance);
}

int main() {
  TiltConfig config;
  config.pitchSign = -1;  // Same adapter used by the sketch.
  TiltController tilt(config);
  level();
  assert(!tilt.latest().valid);
  assert(tilt.begin());
  assert(tilt.status() == TiltStatus::Calibrating);
  assert(!tilt.latest().valid);
  // Moving during calibration must not accumulate a false neutral pose.
  mockGyro.gyro.x = 0.2f;
  tick(tilt, 250);
  assert(tilt.calibrationSamples() == 0);
  level();
  tick(tilt, 200);
  assert(tilt.latest().valid);
  near(tilt.latest().pitch, 0);
  near(tilt.latest().roll, 0);

  // Both signs of each original settled accelerometer direction, no axis swap.
  for (int sign : {-1, 1}) {
    level();
    mockAcceleration.acceleration.x = sign * 9.80665f * 0.5f;
    mockAcceleration.acceleration.z = 9.80665f * sqrtf(0.75f);
    tick(tilt, 200);
    near(tilt.latest().pitch, sign * 30.0f / 45);
    near(tilt.latest().roll, 0);
    level();
    tick(tilt, 200);
    mockAcceleration.acceleration.y = sign * 9.80665f * 0.5f;
    mockAcceleration.acceleration.z = 9.80665f * sqrtf(0.75f);
    tick(tilt, 200);
    near(tilt.latest().roll, sign * 30.0f / 45);
    near(tilt.latest().pitch, 0);
  }
  level();
  tick(tilt, 200);
  mockGyro.gyro.z = 0.785398163f;  // +45 deg/s, original positive yaw.
  tick(tilt);
  near(tilt.latest().yaw, 0.5f);
  mockGyro.gyro.z = -0.785398163f;
  tick(tilt);
  near(tilt.latest().yaw, -0.5f);
  mockGyro.gyro.z = 0;
  tick(tilt);
  near(tilt.latest().yaw, 0);

  // Hold mode: yawHold follows the twist angle, so it stays on while held.
  // The 1-sample rate pulses above twisted about 0.45 deg each way; re-zero first.
  tilt.recenterYaw();
  near(tilt.latest().yawHold, 0);
  for (int sign : {1, -1}) {
    mockGyro.gyro.z = sign * 0.785398163f;  // 45 deg/s for 0.5 s = 22.5 deg.
    tick(tilt, 50);
    near(tilt.latest().yawDeg, sign * 22.5f, 0.5f);
    near(tilt.latest().yawHold, sign * 0.5f, 0.02f);
    mockGyro.gyro.z = 0;
    tick(tilt, 50);  // Holding still keeps yawing, while the rate output stops.
    near(tilt.latest().yawHold, sign * 0.5f, 0.02f);
    near(tilt.latest().yaw, 0);
    mockGyro.gyro.z = -sign * 0.785398163f;  // Turn back to the front.
    tick(tilt, 50);
    mockGyro.gyro.z = 0;
    tick(tilt);
    near(tilt.latest().yawHold, 0);
  }
  mockGyro.gyro.z = 0.785398163f;  // A 4.5 deg twist is inside the deadzone.
  tick(tilt, 10);
  mockGyro.gyro.z = 0;
  tick(tilt);
  near(tilt.latest().yawHold, 0, 0.0001f);
  mockGyro.gyro.z = 0.785398163f;
  tick(tilt, 50);
  mockGyro.gyro.z = 0;
  tick(tilt);
  assert(tilt.latest().yawHold > 0.4f);
  tilt.recenterYaw();  // The game's START event re-zeroes yaw only.
  near(tilt.latest().yawHold, 0, 0.0001f);
  near(tilt.latest().pitch, 0);

  mockAcceleration.acceleration.y = 9.80665f * 0.5f;
  mockAcceleration.acceleration.z = 9.80665f * sqrtf(0.75f);
  tick(tilt, 200);
  assert(tilt.recenter());
  near(tilt.latest().roll, 0);
  mockTimeUs += 151000;
  assert(tilt.status() == TiltStatus::Stale);
  assert(!tilt.latest().valid && tilt.latest().roll == 0);
  tick(tilt);
  assert(tilt.latest().valid);
  mockAcceleration = sensors_event_t();  // Freefall suppresses input.
  tick(tilt);
  assert(!tilt.latest().valid);
  level();
  tick(tilt);
  assert(tilt.latest().valid);
  mockPresent = false;
  tick(tilt);
  assert(tilt.status() == TiltStatus::Fault);
  assert(!tilt.latest().valid && tilt.latest().yaw == 0 && tilt.latest().yawHold == 0);
  mockPresent = true;
  tick(tilt);
  assert(!tilt.latest().valid);  // Reconnection requires explicit begin.
  assert(tilt.begin());
  tick(tilt, 230);
  assert(tilt.latest().valid);
  // Sample clock wrap must not make a valid sensor freeze.
  mockTimeUs = 0xffffffffULL - 5000;
  tick(tilt, 2);
  assert(tilt.latest().valid);

  ButtonEvents button;
  button.edge(0);
  button.edge(1);
  button.edge(49);
  assert(button.count() == 1);
  button.edge(50);
  button.edge(100);
  assert(button.count() == 3);  // Several presses survive between sends.
  ButtonEvents wrapping;
  wrapping.edge(0xfffffff0U);
  wrapping.edge(0x20U);
  assert(wrapping.count() == 1);
  wrapping.edge(0x30U);
  assert(wrapping.count() == 2);
  puts("Tilt directions, yaw hold, calibration, staleness, faults, recenter and button tests passed");
}
