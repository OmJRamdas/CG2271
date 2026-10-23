/*
 * CG2271 A4 — rotary encoder decoder (see encoder.h)
 */

#include "encoder.h"

static int clampLevel(int v)
{
    return v < 0 ? 0 : (v > 100 ? 100 : v);
}

void encoder_init(enc_t *e, uint8_t pins, bool reverse)
{
    e->reverse     = reverse;
    e->level       = ENC_INITIAL;
    e->accumulated = 0;
    e->skipped     = 0;
    e->previous    = pins & 3U;
}

bool encoder_update(enc_t *e, uint8_t pins)
{
    /* Index (previous << 2 | current) -> -1, 0, +1 quarter steps. */
    static const int8_t TABLE[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };

    pins &= 3U;
    if (pins == e->previous) return false;          /* bounce back to same state */
    if ((pins ^ e->previous) == 3U) {                /* both bits changed: missed */
        e->accumulated = 0;
        e->previous = pins;
        e->skipped++;
        return false;
    }

    int movement = TABLE[(e->previous << 2) | pins];
    e->previous = pins;
    if (e->reverse) movement = -movement;
    e->accumulated += movement;

    int old = e->level;
    if (e->accumulated >= ENC_TRANSITIONS_STEP) {
        e->accumulated = 0;
        e->level = clampLevel(e->level + ENC_STEP);
    } else if (e->accumulated <= -ENC_TRANSITIONS_STEP) {
        e->accumulated = 0;
        e->level = clampLevel(e->level - ENC_STEP);
    }
    return e->level != old;
}
