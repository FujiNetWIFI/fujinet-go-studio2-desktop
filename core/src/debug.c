/*
 * debug.c -- the session's side of the debugger: one engine per session
 * (core/debugger/debugger.c), told when a power cycle takes the machine
 * away and brings a new one up, and fed FujiNet's traffic from the start.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "session_internal.h"
#include "debugger_internal.h"

studio2debug *studio2debug_get(studio2session *s)
{
    if (!s)
        return NULL;
    if (!s->debugger)
        s->debugger = studio2debug_create();
    return s->debugger;
}

/* The FujiNet trace is always on: it is cheap, and it is most useful about
 * what happened before anyone thought to look. */
void studio2debug_trace_fujinet(struct studio2session *s)
{
    studio2debug_fujinet_install(studio2debug_get(s));
}

void studio2debug_before_power_cycle(struct studio2session *s)
{
    if (s->debugger)
        studio2debug_power_down(s->debugger);
}

void studio2debug_after_power_cycle(struct studio2session *s)
{
    if (!s->debugger)
        return;
    studio2debug_set_cart_path(s->debugger, s->cart_path);
    studio2debug_power_up(s->debugger);
}

void studio2debug_destroy(struct studio2session *s)
{
    studio2debug_free(s->debugger);
    s->debugger = NULL;
}
