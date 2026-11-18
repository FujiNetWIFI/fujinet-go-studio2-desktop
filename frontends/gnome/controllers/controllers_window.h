/*
 * The Controllers window -- the RCA Studio II's top panel drawn as a player
 * sees it: two recessed keypads (A and B), the gold plate, the ribbed panel
 * with its LED and the CLEAR bar, and the CONFIG and STOP buttons. Held keys
 * light up in the accent whoever holds them (keyboard, gamepad, this window),
 * the pointer presses every control, and Map mode rebinds any of them to a key
 * or a gamepad button.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "studio2session.h"

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. `parent` is only used the first
 * time, to set the transient-for relationship. */
void studio2_controllers_toggle(GtkWindow *parent, studio2session *session);
/* Shows it (or raises it). */
void studio2_controllers_show(GtkWindow *parent, studio2session *session);
gboolean studio2_controllers_visible(void);
