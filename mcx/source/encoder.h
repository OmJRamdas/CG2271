/*
 * CG2271 A4 — rotary encoder (quadrature) decoder -> 0..100 throttle level
 *
 * C port of esp32/esp32_flight/EncoderThrottle.h (same table and defaults).
 * Feed it the 2-bit pin state ((CLK << 1) | DT) on every pin change.
 * Pure C, no hardware or RTOS dependencies, so it can be unit-tested on a PC.
 * On the MCX, encoder_update() runs in the PORTA ISR; read the level from a
 * task inside a critical section.
 */

#ifndef ENCODER_H_
#define ENCODER_H_

#include <stdbool.h>
#include <stdint.h>

#define ENC_STEP               5    /* level change per detent (percent)        */
#define ENC_TRANSITIONS_STEP   4    /* quadrature transitions per detent        */
#define ENC_INITIAL           50

typedef struct {
    bool     reverse;         /* flip direction if the knob turns the wrong way */
    int      level;           /* 0..100                                         */
    int      accumulated;     /* transitions towards the next detent            */
    uint32_t skipped;         /* missed transitions (both bits changed at once) */
    uint8_t  previous;        /* last 2-bit pin state                           */
} enc_t;

/* Latch the idle pin state before enabling the pin interrupts. */
void encoder_init(enc_t *e, uint8_t pins, bool reverse);

/* Process one pin change. Returns true if the level changed. */
bool encoder_update(enc_t *e, uint8_t pins);

#endif /* ENCODER_H_ */
