/*
 * input_test -- the merged input layer through the public API: every source
 * (keyboard, the Controllers window's pointer) holds a target by its own bit,
 * so one letting go never releases the other (a panel release used to clear
 * a keyboard hold of the same key), and CLEAR -- which holds the CPU in reset
 * while down -- is released reliably by studio2session_release_pointer, and
 * independently of a keyboard CLEAR. Read back both from the held queries and
 * from the machine (the counter app's RAM).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "studio2session.h"
#include "session_internal.h"
#include "studio2debug.h"
#include "test_tmpdir.h"
#include "test_rom.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint8_t ram(studio2session *s, uint16_t addr)
{
    studio2debug *d = studio2session_debugger(s);
    uint8_t v = 0;
    studio2debug_attach(d);
    studio2debug_read(d, addr, &v, 1);
    studio2debug_detach(d);
    return v;
}

static int moving(studio2session *s)
{
    uint8_t a = ram(s, TEST_RAM_COUNTER);
    sleep_ms(200);
    return ram(s, TEST_RAM_COUNTER) != a;
}

#define KEY(p, a) STUDIO2_TARGET_PORT(p, a)
#define CLEAR STUDIO2_TARGET_SWITCH(STUDIO2_SW_CLEAR)

int main(void)
{
    char cfg[512], data[512], app[700];
    studio2session_paths p;
    studio2session *s;
    studio2session_start_opts o;
    int i;

    test_tmpdir(cfg, sizeof cfg, "icfg");
    test_tmpdir(data, sizeof data, "idata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    snprintf(app, sizeof app, "%s/counter.bin", cfg);
    check(test_rom_write(app, test_counter_app, sizeof test_counter_app) > 0, "wrote a FujiNet app");

    s = studio2session_new(&p);
    if (!s) return 1;
    studio2session_default_opts(s, &o);
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    if (studio2session_start(s, &o) != 0 || studio2session_load_cart(s, app) != 0) {
        printf("FAIL: start: %s\n", studio2session_last_error(s));
        return 1;
    }
    for (i = 0; i < 1000 && !studio2session_cart_booted_game(s); i++) sleep_ms(5);
    check(moving(s), "the app runs");

    /* a pointer press and a keyboard hold of the same key are two sources */
    check(studio2session_key(s, 'q', 1) == 1, "Q holds keypad A 1");
    studio2session_pointer_press(s, KEY(0, STUDIO2_ACT_1), 1);
    studio2session_pointer_press(s, KEY(0, STUDIO2_ACT_1), 0);
    check(studio2session_buttons_held(s, 0) == (1u << STUDIO2_ACT_1),
          "the panel's release leaves the keyboard's hold");
    sleep_ms(150);
    check(ram(s, TEST_RAM_KEY1) == 1, "and the machine still sees the key down");
    studio2session_pointer_press(s, KEY(0, STUDIO2_ACT_1), 1);
    studio2session_release_all(s);
    check(studio2session_buttons_held(s, 0) == (1u << STUDIO2_ACT_1) && studio2session_target_held(s, KEY(0, STUDIO2_ACT_1)),
          "release_all lets go of the keyboard only");
    studio2session_release_pointer(s);
    check(studio2session_buttons_held(s, 0) == 0, "release_pointer lets go of the pointer");
    sleep_ms(150);
    check(ram(s, TEST_RAM_KEY1) == 0, "and the key is up in the machine");

    /* the other keypad is its own */
    studio2session_pointer_press(s, KEY(1, STUDIO2_ACT_3), 1);
    check(studio2session_buttons_held(s, 1) == (1u << STUDIO2_ACT_3) && studio2session_buttons_held(s, 0) == 0,
          "keypad B 3 is keypad B's");
    studio2session_release_pointer(s);

    /* CLEAR: a pointer hold freezes the CPU and release_pointer restores it */
    studio2session_pointer_press(s, CLEAR, 1);
    check(studio2session_switch_held(s, STUDIO2_SW_CLEAR) && studio2session_target_held(s, CLEAR),
          "a pointer CLEAR is held");
    sleep_ms(200);
    check(!moving(s), "and the CPU is in reset");
    studio2session_release_pointer(s);
    check(!studio2session_switch_held(s, STUDIO2_SW_CLEAR), "release_pointer lets CLEAR go");
    sleep_ms(200);
    check(moving(s), "and the CPU runs again");

    /* the keyboard's CLEAR and the pointer's are independent */
    check(studio2session_key(s, STUDIO2_KEYSYM_F3, 1) == 1, "F3 holds CLEAR");
    studio2session_pointer_press(s, CLEAR, 1);
    studio2session_release_pointer(s);
    check(studio2session_switch_held(s, STUDIO2_SW_CLEAR), "the pointer's release leaves the keyboard's CLEAR");
    studio2session_pointer_press(s, CLEAR, 1);
    check(studio2session_key(s, STUDIO2_KEYSYM_F3, 0) == 1 && studio2session_switch_held(s, STUDIO2_SW_CLEAR),
          "the keyboard's release leaves the pointer's CLEAR");
    studio2session_release_all(s);
    check(studio2session_switch_held(s, STUDIO2_SW_CLEAR), "release_all (keyboard only) leaves the pointer's CLEAR");
    studio2session_release_pointer(s);
    check(!studio2session_switch_held(s, STUDIO2_SW_CLEAR), "and release_pointer lets it go");
    sleep_ms(200);
    check(moving(s), "the machine runs");

    /* two pads on one keypad, and a pad's button beside its stick, are
     * separate sources too (the gamepad thread's entry points) */
    session_gamepad_target(s, 0, KEY(0, STUDIO2_ACT_5), 1);
    session_gamepad_target(s, 1, KEY(0, STUDIO2_ACT_5), 1);
    session_gamepad_release(s, 0);
    check(studio2session_buttons_held(s, 0) == (1u << STUDIO2_ACT_5), "pad 1 still holds the key after pad 0 lets go");
    session_gamepad_release(s, 1);
    check(studio2session_buttons_held(s, 0) == 0, "and nothing once both have");
    session_gamepad_target(s, 0, KEY(0, STUDIO2_ACT_6), 1);
    session_gamepad_apply(s, 0, 0, STUDIO2_ACT_6, 1);
    session_gamepad_target(s, 0, KEY(0, STUDIO2_ACT_6), 0);
    check(studio2session_buttons_held(s, 0) == (1u << STUDIO2_ACT_6), "a pad's stick keeps the key after its button lets go");
    session_gamepad_apply(s, 0, 0, STUDIO2_ACT_6, 0);
    check(studio2session_buttons_held(s, 0) == 0, "and releases it");
    session_gamepad_target(s, 0, CLEAR, 1);
    check(studio2session_key(s, STUDIO2_KEYSYM_F3, 1) == 1, "F3 holds CLEAR beside a pad's CLEAR");
    session_gamepad_release(s, 0);
    check(studio2session_switch_held(s, STUDIO2_SW_CLEAR), "the pad's release leaves the keyboard's CLEAR");
    studio2session_release_all(s);
    check(!studio2session_switch_held(s, STUDIO2_SW_CLEAR), "and the keyboard's lets it go");
    session_gamepad_target(s, 99, KEY(0, STUDIO2_ACT_1), 1);
    session_gamepad_target(s, 0, STUDIO2_TARGET_SYSACT(0), 1);
    check(studio2session_buttons_held(s, 0) == 0, "an owner out of range, or a system action, holds nothing");
    sleep_ms(200);
    check(moving(s), "the machine runs");

    /* a system action fires on the press and holds nothing */
    studio2session_pointer_press(s, STUDIO2_TARGET_SYSACT(STUDIO2_SYSACT_RESET_CONFIG), 0);
    check(studio2session_cart_booted_game(s), "a system action's release does nothing");

    studio2session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
