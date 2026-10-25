/*
 * mame_trace_test -- the core held to MAME, read for read, with no RCA code:
 * every bus read (the cycle it starts at, the address, the byte) the CPU
 * and the CDP1861's DMA make, folded into a count and a hash, against the
 * same fold of MAME's own run (tools/ab/abtrace.lua on a studio2 build with
 * the firmware's FujiNet graft; recipe in tools/ab/README.md):
 *
 *   game     the tests' BIOS (testbios.asm, loaded into MAME as its ROMs)
 *            and the counter as a plain game, on MAME's stock cart and here
 *            through the FujiNet cart: the interrupt, the DMA bursts, EF1,
 *            the keypads through OUT 2 / EF3 / EF4, two key presses;
 *   config   the stand-in (stub.asm, likewise MAME's ROMs) booting the
 *            baked CONFIG through MAME's FujiNet device with the link down:
 *            the cart-served raster by DMA timing, the text engine's
 *            hotspots, the mailbox.
 *
 * Both runs use MAME's power-on (no CLEAR pulse, IE = 0). A difference
 * prints where to look: rerun tools/ab/mame_ab.py, or diff the two traces.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "studio2_internal.h"
#include "test_rom.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

typedef struct {
    uint64_t lo, hi;            /* the cycles counted: [lo, hi) */
    uint64_t hash;
    unsigned long count;
} fold_t;

static void fold_hook(studio2_machine_t *m, void *user, int kind, uint16_t addr, uint8_t data)
{
    fold_t *f = user;
    char line[40];
    (void)kind;
    if (m->cycles < f->lo || m->cycles >= f->hi)
        return;
    snprintf(line, sizeof line, "%llu %04X %02X\n", (unsigned long long)m->cycles, addr, data);
    for (const char *p = line; *p; p++)
        f->hash = (f->hash ^ (uint8_t)*p) * 0x100000001b3ULL;
    f->count++;
}

typedef struct {
    int frame, pad, key, down;
} key_ev;

static void run(studio2_machine_t *m, fold_t *f, int frames, const key_ev *ev, int nev)
{
    f->hash = 0xcbf29ce484222325ULL;
    f->count = 0;
    m->bus_hook = fold_hook;
    m->bus_hook_user = f;
    m->watch = STUDIO2_WATCH_MEMR | STUDIO2_WATCH_DMA;
    for (int fr = 0; fr < frames; fr++)
    {
        for (int i = 0; i < nev; i++)
            if (ev[i].frame == fr)
            {
                if (ev[i].down)
                    m->keys_live[ev[i].pad] |= (uint16_t)(1u << ev[i].key);
                else
                    m->keys_live[ev[i].pad] &= (uint16_t)~(1u << ev[i].key);
            }
        studio2_machine_latch_inputs(m);
        studio2_machine_run_frame(m);
        if (m->cart)
            studio2_cart_frame(m->cart);
        m->out_frames = 0;
    }
}

int main(void)
{
    static studio2_machine_t m;
    studio2_cart_t *c = studio2_cart_create();
    char why[200], what[200];
    fold_t f;

    /* the game: MAME's keys at its frame notifiers 60/70/90/95 land two
     * frames later (Lua sets the field after that frame's input update) */
    {
        static const key_ev keys[] = {
            { 62, STUDIO2_KEYPAD_A, 1, 1 }, { 72, STUDIO2_KEYPAD_A, 1, 0 },
            { 92, STUDIO2_KEYPAD_B, 3, 1 }, { 97, STUDIO2_KEYPAD_B, 3, 0 },
        };
        studio2_cart_power_on(c, test_counter_game, sizeof test_counter_game, "127.0.0.1:1",
                              true, why, sizeof why);
        studio2_machine_init(&m, test_bios, sizeof test_bios, c, true);
        f.lo = 10ull * STUDIO2_FRAME_CLOCKS;
        f.hi = 121ull * STUDIO2_FRAME_CLOCKS;
        run(&m, &f, 122, keys, 4);
        snprintf(what, sizeof what, "game: %lu reads, hash %016llX, as MAME's (347495, F00E06BEC84D1E63)",
                 f.count, (unsigned long long)f.hash);
        check(f.count == 347495 && f.hash == 0xF00E06BEC84D1E63ULL, what);
    }

    /* CONFIG on the stand-in, the link down, the mailbox inline */
    {
        studio2_cart_power_on(c, NULL, 0, "127.0.0.1:1", true, why, sizeof why);
        studio2_machine_init(&m, NULL, 0, c, true);
        f.lo = 1ull * STUDIO2_FRAME_CLOCKS;
        f.hi = 300ull * STUDIO2_FRAME_CLOCKS;
        run(&m, &f, 301, NULL, 0);
        snprintf(what, sizeof what, "config: %lu reads, hash %016llX, as MAME's (805812, 1BC6C118D85EF4AF)",
                 f.count, (unsigned long long)f.hash);
        check(f.count == 805812 && f.hash == 0x1BC6C118D85EF4AFULL, what);
    }

    studio2_cart_destroy(c);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
