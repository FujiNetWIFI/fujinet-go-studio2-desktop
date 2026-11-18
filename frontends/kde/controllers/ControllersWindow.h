/*
 * The Controllers window (F9): the RCA Studio II's console top drawn from the
 * session's layout -- both 10-key keypads in their wells, the centre panel with
 * the CLEAR bar and its LED, and the CONFIG and STOP buttons -- plus the
 * gamepads with their keypad assignments, with Map mode for rebinding any
 * control. Hidden at start; a fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

extern "C" {
#include "studio2session.h"
}

namespace studio2ctl {
/* The drawing's metrics, shared with the offscreen test: pixels per layout
 * unit (a keypad key's height), the case's margin round the controls, and where
 * the layout's origin sits down from the top of the window. */
constexpr double kUnit = 32.0;
constexpr int kCasePad = 14;
constexpr int kTop = 14;
} // namespace studio2ctl

/* Shows the window, or hides it when it is showing. */
void studio2ControllersToggle(QWidget *parent, studio2session *session);
/* Shows (and raises) the window; leaves it showing if it already is. */
void studio2ControllersShow(QWidget *parent, studio2session *session);
bool studio2ControllersVisible();
/* The Controllers window has the keyboard focus (the main window then keeps
 * its keys down when it loses activation). */
bool studio2ControllersActive();
/* Preferences closed or the session restarted: redraw. */
void studio2ControllersRefresh();
