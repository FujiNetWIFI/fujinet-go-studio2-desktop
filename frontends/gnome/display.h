/*
 * The emulator display: a GtkWidget that pulls frames from the session on
 * the compositor's own frame clock.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "studio2session.h"

#define STUDIO2_TYPE_DISPLAY (studio2_display_get_type())
G_DECLARE_FINAL_TYPE(Studio2Display, studio2_display, STUDIO2, DISPLAY, GtkWidget)

GtkWidget *studio2_display_new(studio2session *session);
/* The picture a television showed (the whole 100 x 246 area at 4:3), or
 * square pixels (a very tall 100:246). */
void studio2_display_set_tv_aspect(Studio2Display *self, gboolean tv);
void studio2_display_set_smooth(Studio2Display *self, gboolean smooth);
