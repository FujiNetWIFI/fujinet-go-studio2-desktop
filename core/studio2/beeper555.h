/* beeper555.h -- the Studio II's Q-gated NE555 beeper.
 *
 * The console does not sound a fixed tone. Q gates an NE555 astable whose
 * control pin sits on a 10 uF capacitor, so a note starts near 625 Hz,
 * holds for about 20 ms, then glides down to about 502 Hz over some 200 ms;
 * Q low lets the pitch recover while the output fades over about 96 ms, and
 * a quick retrigger picks up from where the recovery got to. The output is
 * not square: high and low run 11:6, from Ra = 400k, Rb = 480k.
 *
 * This is a clock-for-clock C port of the behavioural model in the RCA
 * Studio II core for MiSTer (rtl/studio2_beeper_inline.svh), fitted to
 * retail console recordings, the December 1976 RCA demonstration unit and
 * FliP's Q-Sound Test cartridge; the evidence is in that repository's
 * docs/beeper-status.md. It ticks once per CPU clock (1.76 MHz).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * (from StudioII_MiSTer, Elle Ball; GPL-3.0-or-later as part of this app)
 */

#ifndef STUDIO2_BEEPER555_H
#define STUDIO2_BEEPER555_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The tuning choices of the MiSTer core's OSD. Consoles vary (an RC
 * oscillator); Original is the RCA demonstration unit. */
enum {
    BEEPER555_ORIGINAL = 0,
    BEEPER555_HIGH, BEEPER555_HIGHER, BEEPER555_HIGHEST,
    BEEPER555_LOWEST, BEEPER555_LOWER, BEEPER555_LOW
};

typedef struct {
    uint16_t half;          /* audible oscillator period */
    uint16_t drive_half;    /* fresh Q-high contour */
    uint16_t control_half;  /* recovered control state for a retrigger */
    uint16_t cnt;
    uint16_t cycle_base;    /* period for one high/low pair */
    uint16_t cycle_scale;   /* tuning, held for the same pair (Q14) */
    uint16_t curve_cnt;
    uint16_t control_cnt;
    uint16_t on_ticks;
    uint16_t amp_cnt;
    uint16_t track_cnt;
    uint16_t eb_frac;
    uint8_t amp;
    uint8_t q_prev;
    uint8_t out;
} beeper555_t;

void beeper555_reset(beeper555_t *b);

/* One CPU clock with Q as given. */
void beeper555_tick(beeper555_t *b, int q, int tuning);

/* The output now, -1.0 .. 1.0. */
static inline float beeper555_level(const beeper555_t *b)
{
    float mag = b->amp * (1.0f / 255.0f);
    return b->out ? mag : -mag;
}

/* The pitch the oscillator is running at (or would start at), in Hz. */
int beeper555_hz(const beeper555_t *b, uint32_t clock_hz);

#ifdef __cplusplus
}
#endif

#endif /* STUDIO2_BEEPER555_H */
