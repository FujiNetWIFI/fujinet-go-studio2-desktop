/*
 * beeper_test -- the NE555 beeper (beeper555.c) held to the console.
 *
 * The figures are the console's, from retail recordings, the December 1976
 * RCA demonstration unit and FliP's Q-Sound Test cartridge (StudioII_MiSTer
 * docs/beeper-status.md): a fresh note near 625 Hz that holds about 20 ms
 * and settles near 502 Hz in some 210-220 ms, an 11:6 high/low duty, a
 * release that fades over about 96 ms without resetting the oscillator, a
 * retrigger that resumes from the recovering pitch, and the seven tunings.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>

#include "beeper555.h"

#define CLOCK 1760000.0

static int failures;

static void check(int ok, const char *what)
{
    if (!ok)
    {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static beeper555_t b;
static long now;                      /* clocks since the reset */
static long rise[2], fall;            /* the last two rising edges, last fall */
static long high_len, low_len;        /* the last high and low phases */
static int tuning;

static void run(int q, double ms)
{
    long end = now + (long)(ms * CLOCK / 1000.0);

    while (now < end)
    {
        int was = b.out;
        beeper555_tick(&b, q, tuning);
        now++;
        if (!was && b.out)
        {
            low_len = fall ? now - fall : 0;
            rise[0] = rise[1];
            rise[1] = now;
        }
        else if (was && !b.out)
        {
            high_len = now - rise[1];
            fall = now;
        }
    }
}

/* the pitch of the last complete cycle */
static double hz(void)
{
    return rise[0] && rise[1] > rise[0] ? CLOCK / (double)(rise[1] - rise[0]) : 0.0;
}

static void restart(int t)
{
    beeper555_reset(&b);
    now = rise[0] = rise[1] = fall = 0;
    tuning = t;
    run(0, 500);
}

static int near(double v, double want, double tol)
{
    return fabs(v - want) <= tol;
}

int main(void)
{
    char what[160];

    /* the fresh note, its hold and its descent */
    restart(BEEPER555_ORIGINAL);
    run(1, 5);
    double fresh = hz();
    snprintf(what, sizeof what, "fresh pitch %.1f Hz, want 625 +/- 3", fresh);
    check(near(fresh, 625.0, 3.0), what);
    run(1, 13);
    snprintf(what, sizeof what, "held through 18 ms: %.1f Hz", hz());
    check(near(hz(), fresh, 2.0), what);
    run(1, 82);
    double mid = hz();
    snprintf(what, sizeof what, "descending at 100 ms: %.1f Hz", mid);
    check(mid < fresh - 30.0 && mid > 510.0, what);
    run(1, 150);
    double floor_hz = hz();
    snprintf(what, sizeof what, "settled by 250 ms: %.1f Hz, want 502.5 +/- 3", floor_hz);
    check(near(floor_hz, 502.5, 3.0), what);
    run(1, 250);
    snprintf(what, sizeof what, "sustained at 500 ms: %.1f Hz", hz());
    check(near(hz(), floor_hz, 1.0), what);

    /* 11:6, high then low */
    {
        double duty = (double)high_len / (double)(high_len + low_len);
        snprintf(what, sizeof what, "duty %.4f, want 11/17 = %.4f", duty, 11.0 / 17.0);
        check(near(duty, 11.0 / 17.0, 0.002), what);
    }

    /* the release: Q low fades, it does not cut */
    run(0, 40);
    snprintf(what, sizeof what, "still sounding 40 ms after Q low (amp %u)", b.amp);
    check(b.amp > 0, what);
    run(0, 110);
    snprintf(what, sizeof what, "silent 150 ms after Q low (amp %u)", b.amp);
    check(b.amp == 0, what);

    /* retriggers: a longer rest recovers more of the fresh pitch */
    {
        static const double gap_ms[] = {60.2, 105.5, 160.7, 205.9};
        double prev = 0.0;
        for (int i = 0; i < 4; i++)
        {
            restart(BEEPER555_ORIGINAL);
            run(1, 400);                /* a long note: at the floor */
            run(0, gap_ms[i]);
            run(1, 8);                  /* past the retrigger glide */
            double pct = hz() / fresh * 100.0;
            snprintf(what, sizeof what, "retrigger after %.1f ms: %.1f%% of fresh", gap_ms[i], pct);
            check(pct > prev && pct < 100.5 && pct > 82.0, what);
            prev = pct;
        }
        snprintf(what, sizeof what, "a 206 ms rest recovers to %.1f%%, want >= 98", prev);
        check(prev >= 98.0, what);
    }

    /* quick pulses (Speedway) stay near the top */
    restart(BEEPER555_ORIGINAL);
    for (int i = 0; i < 20; i++)
    {
        run(1, 15);
        run(0, 15);
    }
    run(1, 5);
    snprintf(what, sizeof what, "rapid pulses stay near the top: %.1f Hz", hz());
    check(hz() > 600.0, what);

    /* the tunings: fresh and sustained endpoints */
    {
        static const struct { int t; double fresh, floor; } tune[] = {
            {BEEPER555_HIGHEST, 756.1, 607.8},
            {BEEPER555_LOWEST, 516.5, 415.3},
        };
        for (int i = 0; i < 2; i++)
        {
            restart(tune[i].t);
            run(1, 5);
            double f = hz();
            run(1, 495);
            double s = hz();
            snprintf(what, sizeof what, "tuning %d: %.1f/%.1f Hz, want %.1f/%.1f",
                     tune[i].t, f, s, tune[i].fresh, tune[i].floor);
            check(near(f, tune[i].fresh, 4.0) && near(s, tune[i].floor, 4.0), what);
        }
    }

    if (failures)
        printf("%d failure(s)\n", failures);
    else
        printf("beeper: all checks passed\n");
    return failures != 0;
}
