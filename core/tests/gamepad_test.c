/*
 * gamepad_test -- the gamepad layer's pure helpers (no hardware), and that
 * the SDL gamepad subsystem starts and stops cleanly with nothing attached.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "studio2session.h"
#include "gamepad_sdl.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    int assign[4];

    /* stick hysteresis */
    check(studio2_dir_from_stick(0.f, 0.f, 0, 0.35f, 0.15f) == 0, "centered stick is no direction");
    check(studio2_dir_from_stick(0.5f, 0.f, 0, 0.35f, 0.15f) == STUDIO2_DIR_RIGHT, "half right engages right");
    check(studio2_dir_from_stick(0.2f, 0.f, 0, 0.35f, 0.15f) == 0, "a fifth right does not engage");
    check(studio2_dir_from_stick(0.2f, 0.f, STUDIO2_DIR_RIGHT, 0.35f, 0.15f) == STUDIO2_DIR_RIGHT, "but holds once engaged");
    check(studio2_dir_from_stick(0.1f, 0.f, STUDIO2_DIR_RIGHT, 0.35f, 0.15f) == 0, "and releases below exit");
    check(studio2_dir_from_stick(-0.6f, -0.6f, 0, 0.35f, 0.15f) == (STUDIO2_DIR_LEFT | STUDIO2_DIR_UP), "diagonals are two bits");
    check(studio2_dir_from_stick(0.6f, 0.05f, 0, 0.35f, 0.15f) == STUDIO2_DIR_RIGHT,
          "a pure horizontal picks up no vertical noise");

    /* directions as the games' keys */
    check(studio2_key_from_dir(STUDIO2_DIR_UP) == 2 && studio2_key_from_dir(STUDIO2_DIR_DOWN) == 8 &&
          studio2_key_from_dir(STUDIO2_DIR_LEFT) == 4 && studio2_key_from_dir(STUDIO2_DIR_RIGHT) == 6,
          "up, down, left, right are 2, 8, 4, 6");
    check(studio2_key_from_dir(STUDIO2_DIR_UP | STUDIO2_DIR_LEFT) == 1 &&
          studio2_key_from_dir(STUDIO2_DIR_UP | STUDIO2_DIR_RIGHT) == 3 &&
          studio2_key_from_dir(STUDIO2_DIR_DOWN | STUDIO2_DIR_LEFT) == 7 &&
          studio2_key_from_dir(STUDIO2_DIR_DOWN | STUDIO2_DIR_RIGHT) == 9,
          "a diagonal is its corner key");
    check(studio2_key_from_dir(STUDIO2_DIR_UP | STUDIO2_DIR_DOWN) == 5 && studio2_key_from_dir(0) == 5,
          "opposites cancel to the centre");

    /* port assignment */
    assign[0] = -1; assign[1] = -1; assign[2] = -1;
    check(studio2_pad_for_port(assign, 2, 0) == 0 && studio2_pad_for_port(assign, 2, 1) == 1,
          "connection order: first pad keypad A, second keypad B");
    check(studio2_pad_for_port(assign, 1, 1) == -1, "one pad leaves keypad B to the keyboard");
    assign[0] = 1; assign[1] = -1;
    check(studio2_pad_for_port(assign, 2, 1) == 0 && studio2_pad_for_port(assign, 2, 0) == 1,
          "an explicit assignment wins and the other pad fills the gap");
    assign[0] = 1; assign[1] = 1;
    check(studio2_pad_for_port(assign, 2, 1) == 0 && studio2_pad_for_port(assign, 2, 0) == -1,
          "two pads on one port: the first wins, the other port stays empty");

    /* SDL button tables round-trip */
    {
        int b, ok = 1;
        for (b = 0; b < STUDIO2_PAD_BTN_LEFT_TRIGGER; b++) {
            int sdl = studio2_sdl_button_from_pad(b);
            if (sdl < 0 || studio2_pad_button_from_sdl((uint8_t)sdl) != b) ok = 0;
        }
        check(ok, "every named button round-trips through the SDL tables");
        check(studio2_pad_button_from_sdl(200) == STUDIO2_PAD_BTN_NONE, "an unknown SDL button is NONE");
    }

    /* the subsystem with no hardware: a session with gamepads enabled
     * starts, reports zero pads, and stops */
    {
        char cfg[512], data[512];
        studio2session_paths p;
        studio2session *s;
        studio2session_start_opts o;
        test_tmpdir(cfg, sizeof cfg, "gcfg");
        test_tmpdir(data, sizeof data, "gdata");
        memset(&p, 0, sizeof p);
        p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
        s = studio2session_new(&p);
        studio2session_default_opts(s, &o);
        o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 1;
        check(studio2session_start(s, &o) == 0, "session starts with the gamepad thread");
        check(studio2session_gamepad_count(s) >= 0, "gamepad count answers");
        check(studio2session_gamepad_effective_port(s, 99) == -1, "an absent pad has no port");
        studio2session_gamepad_capture_begin(s);
        check(studio2session_gamepad_capture_poll(s, NULL) == 0, "nothing captured with no pad");
        studio2session_gamepad_capture_cancel(s);
        studio2session_stop(s);
        studio2session_free(s);
        check(1, "session with gamepads stops cleanly");
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
