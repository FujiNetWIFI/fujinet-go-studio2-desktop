/*
 * The Win32 Controllers window (F9) -- see controllers_window.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef STUDIO2_WIN_CONTROLLERS_WINDOW_H
#define STUDIO2_WIN_CONTROLLERS_WINDOW_H

#include <windows.h>

#include "studio2session.h"

/* Show / hide the singleton window (F9, View > Controllers). Created on first
 * use, hidden and not destroyed after. */
void studio2_controllers_toggle(HWND owner, studio2session *session);
/* Show it (and raise it) if it is not showing. */
void studio2_controllers_show(HWND owner, studio2session *session);
BOOL studio2_controllers_visible(void);

/* The gamepad set or the bindings changed: repaint and rebuild the pad rows. */
void studio2_controllers_refresh(void);

#endif
