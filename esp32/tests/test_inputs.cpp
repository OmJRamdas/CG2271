#include <assert.h>
#include <stdio.h>
#include "EncoderThrottle.h"
#include "FireHold.h"

// Gray-code states for one detent: 11 -> 01 -> 00 -> 10 -> 11 raises the level.
// Which physical direction that is depends on the module; REVERSE flips it.
static const uint8_t CW[4] = {1, 0, 2, 3};
static const uint8_t CCW[4] = {2, 0, 1, 3};

static void turn(EncoderThrottle &e, const uint8_t *seq, int detents = 1) {
  for (int d = 0; d < detents; ++d)
    for (int i = 0; i < 4; ++i) e.update(seq[i]);
}

int main() {
  EncoderThrottle e;
  e.begin(3);
  assert(e.level() == 50);
  turn(e, CW);
  assert(e.level() == 55);
  turn(e, CCW, 2);
  assert(e.level() == 45);

  e.update(3);  // Repeated identical state is ignored.
  assert(e.level() == 45);

  // Clamps at both ends.
  turn(e, CW, 30);
  assert(e.level() == 100);
  turn(e, CCW, 30);
  assert(e.level() == 0);

  // A partial wiggle that returns to the same detent does not change the level.
  EncoderThrottle w;
  w.begin(3);
  w.update(2); w.update(3); w.update(1); w.update(3);
  assert(w.level() == 50);

  // A skipped transition resets the accumulator and is counted.
  EncoderThrottle s;
  s.begin(3);
  s.update(2);
  s.update(1);  // 10 -> 01 changes both bits.
  assert(s.skipped() == 1 && s.level() == 50);

  // Reversed direction.
  EncoderConfig reversed;
  reversed.reverse = true;
  EncoderThrottle r(reversed);
  r.begin(3);
  turn(r, CW);
  assert(r.level() == 45);

  // Fire hold: needs 20 ms stable, short glitches never register.
  FireHold f;
  f.update(0, false);
  assert(!f.held());
  f.update(10, true);
  f.update(25, true);
  assert(!f.held());
  f.update(30, true);
  assert(f.held());
  f.update(40, false);  // Bounce on release.
  f.update(45, true);
  f.update(70, true);
  assert(f.held());
  f.update(80, false);
  f.update(99, false);
  assert(f.held());
  f.update(100, false);
  assert(!f.held());
  f.update(200, true);  // Glitch shorter than STABLE_MS.
  f.update(210, false);
  f.update(400, false);
  assert(!f.held());

  puts("input tests passed");
  return 0;
}
