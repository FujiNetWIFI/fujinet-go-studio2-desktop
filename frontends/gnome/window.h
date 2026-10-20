/*
 * The main application window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "studio2session.h"

#define STUDIO2_TYPE_WINDOW (studio2_window_get_type())
G_DECLARE_FINAL_TYPE(Studio2Window, studio2_window, STUDIO2, WINDOW,
                     AdwApplicationWindow)

GtkWidget *studio2_window_new(AdwApplication *app, studio2session *session);
/* main.c: the icon name to use (the installed id, or the in-tree art). */
const char *studio2_icon_name(void);
void studio2_window_toast(Studio2Window *self, const char *text);
/* The display's shape: 0 = the television's 4:3, 1 = square pixels. */
void studio2_window_apply_aspect(Studio2Window *self, int aspect);
/* A dropped or command-line file: a BIOS is imported, a cartridge opened,
 * anything else copied to FujiNet's SD folder. */
void studio2_window_load_media(Studio2Window *self, const char *path);
/* A system action (STUDIO2_SYSACT_*) from any window's keys, with the main
 * window's feedback: Reset to CONFIG toasts, Debugger Stop opens the
 * debugger. */
void studio2_window_run_sysaction(Studio2Window *self, int sysact);
/* Import BIOS: imports `path` and shows studio2session_import_bios's message,
 * which always says what happened, over `over`. Returns the image's index,
 * or -1. */
int studio2_show_bios_import(GtkWidget *over, studio2session *session, const char *path);

/* The accent colour (STUDIO2SESSION_ACCENT_RGB, the console case's tan) as
 * CSS classes every window can use: ".studio2-accent" paints a widget's
 * background in it (with dark text: the tan is pale), ".studio2-accent-text"
 * its label (the darker tan on a light theme), and ".studio2-pill" makes a
 * label a small rounded indicator. Installed once by the main window. */
void studio2_install_accent_css(void);
