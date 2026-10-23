/*
 * Host unit tests for encoder.c — no hardware needed.
 * Mirrors the encoder cases in esp32/tests/test_inputs.cpp.
 *
 *   gcc -std=c11 -Wall -Wextra -I../source test_encoder.c ../source/encoder.c -o test_encoder
 */

#include <stdio.h>

#include "encoder.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* Gray-code states for one detent: 11 -> 01 -> 00 -> 10 -> 11 raises the level. */
static const uint8_t CW[4]  = { 1, 0, 2, 3 };
static const uint8_t CCW[4] = { 2, 0, 1, 3 };

/* Returns how many updates reported a level change. */
static int turn(enc_t *e, const uint8_t *seq, int detents)
{
    int changes = 0;
    for (int d = 0; d < detents; d++)
        for (int i = 0; i < 4; i++)
            if (encoder_update(e, seq[i])) changes++;
    return changes;
}

int main(void)
{
    enc_t e;
    encoder_init(&e, 3, false);
    CHECK(e.level == 50);
    CHECK(turn(&e, CW, 1) == 1);
    CHECK(e.level == 55);
    CHECK(turn(&e, CCW, 2) == 2);
    CHECK(e.level == 45);

    CHECK(!encoder_update(&e, 3));          /* repeated identical state ignored */
    CHECK(e.level == 45);

    turn(&e, CW, 30);                        /* clamps at both ends */
    CHECK(e.level == 100);
    CHECK(turn(&e, CW, 1) == 0);             /* no change reported at the limit */
    turn(&e, CCW, 30);
    CHECK(e.level == 0);

    /* A partial wiggle that returns to the same detent does not change the level. */
    enc_t w;
    encoder_init(&w, 3, false);
    encoder_update(&w, 2); encoder_update(&w, 3); encoder_update(&w, 1); encoder_update(&w, 3);
    CHECK(w.level == 50);

    /* A skipped transition resets the accumulator and is counted. */
    enc_t s;
    encoder_init(&s, 3, false);
    encoder_update(&s, 2);
    CHECK(!encoder_update(&s, 1));           /* 10 -> 01 changes both bits */
    CHECK(s.skipped == 1 && s.level == 50 && s.accumulated == 0);

    /* Reversed direction. */
    enc_t r;
    encoder_init(&r, 3, true);
    turn(&r, CW, 1);
    CHECK(r.level == 45);

    printf("%d/%d encoder checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
