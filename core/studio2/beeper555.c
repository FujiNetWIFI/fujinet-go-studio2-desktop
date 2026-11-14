/* beeper555.c -- the Studio II's Q-gated NE555 beeper (see beeper555.h).
 *
 * Every register update reads the state as it stood at the start of the
 * clock and the last assignment wins, as in the Verilog it comes from.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * (from StudioII_MiSTer, Elle Ball; GPL-3.0-or-later as part of this app)
 */

#include "beeper555.h"

#define HALF_TOP          1400
#define HALF_BOTTOM       1741
#define HOLD_TICKS        35205u   /* ~20 ms */
#define RELEASE_STEP      600u     /* Q-low pitch recovery */
#define RETRIGGER_SETTLE  10561u   /* ~6 ms live-to-control glide */
#define RETRIGGER_TRACK   64u
#define ATTACK_STEP       14u      /* ~2 ms zero to full */
#define DUTY_HIGH_PARTS   11u
#define DUTY_PARTS        17u
#define DUTY_ROUND        8u

static uint16_t tune_scale(int tuning)
{
    switch (tuning)
    {
    case BEEPER555_HIGH:    return 15960;
    case BEEPER555_HIGHER:  return 14978;
    case BEEPER555_HIGHEST: return 13617;
    case BEEPER555_LOWEST:  return 19932;
    case BEEPER555_LOWER:   return 18121;
    case BEEPER555_LOW:     return 17006;
    default:                return 16475;
    }
}

static uint16_t decay_interval(uint16_t h)
{
    if (h < 1443) return 240;
    if (h < 1486) return 280;
    if (h < 1529) return 330;
    if (h < 1572) return 410;
    if (h < 1615) return 520;
    if (h < 1657) return 740;
    if (h < 1699) return 1600;
    if (h < 1715) return 2400;
    if (h < 1727) return 3400;
    if (h < 1735) return 4800;
    if (h < 1740) return 6800;
    return 8191;
}

static uint16_t control_interval(uint16_t h)
{
    if (h >= 1474) return 435;
    if (h >= 1445) return 2750;
    if (h >= 1418) return 3600;
    if (h >= 1410) return 9900;
    if (h >= 1406) return 15000;
    if (h >= 1404) return 25000;
    if (h >= 1402) return 45000;
    return 65000;
}

static uint16_t release_interval(uint8_t a)
{
    if (a >= 192) return 170;
    if (a >= 128) return 240;
    if (a >= 64)  return 400;
    if (a >= 32)  return 800;
    if (a >= 16)  return 1600;
    if (a >= 8)   return 3200;
    return 5700;
}

static uint32_t full_ticks(uint16_t base, uint16_t scale)
{
    return ((uint32_t)base * 2u * scale + 8192u) >> 14;
}

void beeper555_reset(beeper555_t *b)
{
    *b = (beeper555_t){0};
    b->half = b->drive_half = b->control_half = HALF_TOP;
    b->cycle_base = HALF_TOP;
    b->cycle_scale = tune_scale(BEEPER555_ORIGINAL);
}

int beeper555_hz(const beeper555_t *b, uint32_t clock_hz)
{
    uint32_t t = full_ticks(b->cycle_base, b->cycle_scale);
    return t ? (int)((clock_hz + t / 2) / t) : 0;
}

/* the recovered control state steps back toward the fresh pitch */
static void control_recover(const beeper555_t *o, beeper555_t *n)
{
    if (o->control_half > HALF_TOP)
    {
        if (o->control_cnt >= control_interval(o->control_half) - 1u)
        {
            n->control_cnt = 0;
            n->control_half = o->control_half - 1;
        }
        else
            n->control_cnt = o->control_cnt + 1;
    }
    else
        n->control_cnt = 0;
}

void beeper555_tick(beeper555_t *b, int q, int tuning)
{
    const beeper555_t o = *b;
    beeper555_t n = o;

    q = q != 0;
    n.q_prev = (uint8_t)q;

    if (q != o.q_prev)
    {
        n.amp_cnt = 0;
        n.on_ticks = 0;
        n.track_cnt = 0;
        if (q)
        {
            n.curve_cnt = 0;
            n.drive_half = HALF_TOP;
        }
        else
        {
            n.control_half = o.half;
            n.control_cnt = 0;
        }
    }

    if (!q)
    {
        /* released: the pitch recovers, the output fades */
        if (o.half > HALF_TOP)
        {
            if (o.curve_cnt >= RELEASE_STEP - 1u)
            {
                n.curve_cnt = 0;
                n.half = o.half - 1;
            }
            else
                n.curve_cnt = o.curve_cnt + 1;
        }
        else
            n.curve_cnt = 0;

        if (!o.q_prev)
            control_recover(&o, &n);

        if (o.amp != 0)
        {
            if (!o.q_prev)
            {
                if (o.amp_cnt >= release_interval(o.amp) - 1u)
                {
                    n.amp_cnt = 0;
                    n.amp = o.amp - 1;
                }
                else
                    n.amp_cnt = o.amp_cnt + 1;
            }
        }
        else
        {
            n.amp_cnt = 0;
            n.out = 0;
            n.half = o.control_half;
        }
    }
    else
    {
        if (o.q_prev)
        {
            if (o.on_ticks < RETRIGGER_SETTLE)
            {
                /* a retrigger glides from the live pitch to the
                 * recovered one */
                control_recover(&o, &n);
                if (o.half > o.control_half)
                {
                    if (o.track_cnt >= RETRIGGER_TRACK - 1u)
                    {
                        n.track_cnt = 0;
                        n.half = o.half - 1;
                    }
                    else
                        n.track_cnt = o.track_cnt + 1;
                }
                else
                    n.track_cnt = 0;

                if (o.on_ticks >= RETRIGGER_SETTLE - 1u)
                {
                    n.track_cnt = 0;
                    n.half = o.control_half;
                }
            }
            else
            {
                n.control_cnt = 0;
                n.track_cnt = 0;
            }

            if (o.on_ticks < HOLD_TICKS)
            {
                n.on_ticks = o.on_ticks + 1;
                n.curve_cnt = 0;
            }
            else if (o.drive_half < HALF_BOTTOM)
            {
                /* the driven descent */
                if (o.curve_cnt >= decay_interval(o.drive_half) - 1u)
                {
                    n.curve_cnt = 0;
                    n.drive_half = o.drive_half + 1;
                    if (o.drive_half >= o.half)
                    {
                        n.half = o.drive_half + 1;
                        n.control_half = o.drive_half + 1;
                        n.control_cnt = 0;
                    }
                }
                else
                    n.curve_cnt = o.curve_cnt + 1;
            }
            else
            {
                n.drive_half = HALF_BOTTOM;
                if (o.half < HALF_BOTTOM)
                {
                    n.half = HALF_BOTTOM;
                    n.control_half = HALF_BOTTOM;
                }
                n.curve_cnt = 0;
            }
        }

        if (o.q_prev && o.amp < 0xFF)
        {
            if (o.amp_cnt >= ATTACK_STEP - 1u)
            {
                n.amp_cnt = 0;
                n.amp = o.amp + 1;
            }
            else
                n.amp_cnt = o.amp_cnt + 1;
        }
        else
            n.amp_cnt = 0;
    }

    /* the oscillator, its period fixed for each high/low pair */
    if (q || o.amp != 0)
    {
        uint32_t full = full_ticks(o.cycle_base, o.cycle_scale) & 0x1FFFFu;
        uint32_t high = ((full * DUTY_HIGH_PARTS + DUTY_ROUND) / DUTY_PARTS) & 0x1FFFFu;
        uint32_t phase = (o.out ? high : full - high) & 0x1FFFFu;
        uint16_t toggle_at = (uint16_t)(phase - 1u);

        if (o.cnt >= toggle_at)
        {
            n.cnt = 0;
            n.out = !o.out;
            if (!o.out)
            {
                /* at the top a half step every 574/1024 pairs */
                uint16_t eb_sum = o.eb_frac + 574u;
                int eb_long = eb_sum >= 1024u;

                n.cycle_base = (o.half == HALF_TOP && !eb_long) ? HALF_TOP : o.half + 1;
                n.cycle_scale = tune_scale(tuning);
                n.eb_frac = o.half == HALF_TOP ? (eb_sum & 0x3FFu) : 0;
            }
        }
        else
            n.cnt = o.cnt + 1;
    }

    *b = n;
}
