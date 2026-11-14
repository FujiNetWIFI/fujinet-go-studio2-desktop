/*
 * bios_builtin_test -- the frontends' configuration: the public-domain BIOS
 * is built in (linked from studio2_bios_full). A fresh session has it with
 * nothing imported, a game opens on it, and "Use the BIOS" off falls back to
 * the stand-in, which refuses games.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "studio2session.h"
#include "studio2debug.h"
#include "test_tmpdir.h"
#include "test_rom.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    char cfg[512], data[512], game[700];
    studio2session_paths p;
    studio2session_start_opts o;
    studio2debug_io io;
    studio2session *s;

    test_tmpdir(cfg, sizeof cfg, "cfg");
    test_tmpdir(data, sizeof data, "data");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    snprintf(game, sizeof game, "%s/game.bin", cfg);
    check(test_rom_write(game, test_counter_game, sizeof test_counter_game) > 0, "wrote a game");

    s = studio2session_new(&p);
    check(s != NULL, "session created");
    if (!s) return 1;
    check(studio2session_have_bios(s), "the BIOS is built in");
    for (int i = 0; i < studio2session_bios_count(); i++)
        check(!studio2session_bios_available(s, i), "and nothing is imported");

    studio2session_default_opts(s, &o);
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    check(studio2session_start(s, &o) == 0, "session starts");
    check(studio2session_load_cart(s, game) == 0, "a game opens with no import");
    studio2debug_io_get(studio2session_debugger(s), &io);
    check(io.have_bios, "on the real BIOS");
    check(!studio2session_needs_bios(s), "nothing is missing");

    studio2session_set_int(s, "use_bios", 0);
    check(studio2session_reset_to_config(s) == 0, "Use the BIOS off, reset to CONFIG");
    studio2debug_io_get(studio2session_debugger(s), &io);
    check(!io.have_bios, "the stand-in boots");
    check(studio2session_load_cart(s, game) == -1, "and a game is refused");

    studio2session_stop(s);
    studio2session_free(s);
    return failures ? 1 : 0;
}
