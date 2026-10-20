/*
 * stub_test -- the no-BIOS stand-in (core/studio2/stub.asm) and the console
 * around it, with no RCA firmware: CONFIG boots and paints; a FujiNet app
 * (the claim at $07FC) runs, takes its display interrupt every frame and
 * sees keypad A; CLEAR restarts it; a game parks the stand-in and blanks the
 * picture; the same game runs on a BIOS of the tests' own (test_rom.h).
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

static studio2_machine_t m;

static void frames(int n)
{
    for (int i = 0; i < n; i++)
    {
        studio2_machine_latch_inputs(&m);
        studio2_machine_run_frame(&m);
        if (m.cart)
            studio2_cart_frame(m.cart);
        m.out_frames = 0;
    }
}

static int lit(void)
{
    int n = 0;
    for (int i = 0; i < STUDIO2_FB_WIDTH * STUDIO2_FB_HEIGHT; i++)
        n += (m.fb[i] & 0xffffff) != 0;
    return n;
}

static uint8_t ram(uint16_t a)
{
    return studio2_machine_peek(&m, a);
}

static int boot(studio2_cart_t *c, const uint8_t *image, uint32_t len, const uint8_t *bios)
{
    char why[200];
    if (studio2_cart_power_on(c, image, len, "127.0.0.1:1", false, why, sizeof why) != 0)
    {
        printf("  cart: %s\n", why);
        return -1;
    }
    studio2_machine_init(&m, bios, bios ? sizeof test_bios : 0, c, false);
    return 0;
}

int main(void)
{
    studio2_cart_t *c = studio2_cart_create();
    uint8_t a, b;

    /* CONFIG on the stand-in */
    check(boot(c, NULL, 0, NULL) == 0, "the cart powers on with CONFIG");
    frames(90);
    check(!m.have_bios && !m.stub_parked, "no BIOS: the stand-in runs, and is not parked");
    check(lit() > 200, "CONFIG painted its screen through the cart's raster");
    check(m.cpu.r[m.cpu.p] >= 0x0400, "and runs from the cart, past the stand-in");

    /* a FujiNet app */
    check(boot(c, test_counter_app, sizeof test_counter_app, NULL) == 0, "the app is served from power-on");
    frames(60);
    a = ram(TEST_RAM_COUNTER);
    b = ram(TEST_RAM_IRQS);
    frames(10);
    check(ram(TEST_RAM_COUNTER) != a, "the app runs (its counter moves)");
    check((uint8_t)(ram(TEST_RAM_IRQS) - b) == 10, "and takes its display interrupt once a frame");
    check(ram(TEST_RAM_KEY1) == 0, "keypad A 1 reads up");
    m.keys_live[STUDIO2_KEYPAD_A] = 1u << 1;
    frames(2);
    check(ram(TEST_RAM_KEY1) == 1, "and down when held (OUT 2 latches it, EF3 reads it)");
    m.keys_live[STUDIO2_KEYPAD_A] = 1u << 2;
    frames(2);
    check(ram(TEST_RAM_KEY1) == 0, "another key is not key 1");
    m.keys_live[STUDIO2_KEYPAD_A] = 0;
    m.keys_live[STUDIO2_KEYPAD_B] = 1u << 1;
    frames(2);
    check(ram(TEST_RAM_KEY1) == 0, "nor is keypad B's key 1 (EF4)");
    m.keys_live[STUDIO2_KEYPAD_B] = 0;

    /* CLEAR: the CPU sits in reset, the stand-in clears RAM and runs again */
    b = ram(TEST_RAM_IRQS);
    m.clear_live = true;
    frames(3);
    check(m.cpu.mode == CDP1802_MODE_RESET, "CLEAR holds the CPU in reset");
    check(m.vdc.disp == 0, "and resets the 1861 (display off)");
    m.clear_live = false;
    frames(20);
    check(ram(TEST_RAM_IRQS) < 20 && ram(TEST_RAM_IRQS) != b,
          "released, the stand-in starts the app over (RAM cleared, interrupts counting again)");
    check(lit() >= 0 && m.vdc.disp == 1, "with the display back on");

    /* a game: the stand-in cannot run BIOS byte-code */
    check(boot(c, test_counter_game, sizeof test_counter_game, NULL) == 0, "a game is served too");
    frames(30);
    check(m.stub_parked, "the stand-in parks on it");
    check(lit() == 0, "and the picture is blanked");

    /* the same game on a BIOS */
    check(boot(c, test_counter_game, sizeof test_counter_game, test_bios) == 0, "a BIOS imported");
    frames(60);
    a = ram(TEST_RAM_COUNTER);
    frames(5);
    check(m.have_bios && !m.stub_parked, "the BIOS runs, the stand-in is out of the way");
    check(ram(TEST_RAM_COUNTER) != a, "and the game runs");

    studio2_cart_destroy(c);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
