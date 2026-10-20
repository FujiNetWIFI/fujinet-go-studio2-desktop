/*
 * Preferences dialog for the GNOME frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "studio2session.h"

G_BEGIN_DECLS

typedef struct _Studio2Window Studio2Window;

/* Shows the preferences dialog. The analog switch, the volume and the aspect
 * apply live; the BIOS switch and the host options (FujiNet, audio,
 * gamepads) are read when the machine powers up, so the dialog power-cycles
 * the session on close if one of those changed. */
void studio2_prefs_show(Studio2Window *parent, studio2session *session,
                    void (*restart)(Studio2Window *parent));

G_END_DECLS
