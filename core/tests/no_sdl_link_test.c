/*
 * no_sdl_link_test -- the core links and runs on its own: no SDL, no
 * session, no frontend. Links studio2_core alone (a session symbol would fail
 * the link), runs the staged wire codec's self test, plans the baked CONFIG
 * client with the staged planner, and boots a machine for a frame.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "studio2_internal.h"
#include "fujibus.h"
#include "s2map.h"
#include "fujiconfigrom.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    static studio2_machine_t m;
    s2map_plan_t plan;
    char why[160];
    studio2_cart_t *c;

    check(fujibus_selftest(), "the staged fujibus codec passes its self test");
    check(s2map_plan(_configrom, FUJI_CONFIGROM_SIZE, false, &plan) == S2MAP_OK,
          "the staged s2map plans the baked CONFIG client");
    check(plan.claim && plan.st2, "CONFIG is an ST2 that claims the mailbox");

    c = studio2_cart_create();
    check(c != NULL, "the cartridge device creates");
    check(studio2_cart_power_on(c, NULL, 0, "127.0.0.1:1", true, why, sizeof why) == 0,
          "it powers on with CONFIG");
    studio2_machine_init(&m, NULL, 0, c, false);
    studio2_machine_run_frame(&m);
    check(m.frame_count == 1, "a BIOS-less Studio II runs a frame");
    check(m.cycles == STUDIO2_FRAME_CLOCKS, "in exactly one frame's clocks");
    studio2_cart_destroy(c);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
