/*
 * The Controllers panel (F9 / View menu): the Studio II's top drawn from the
 * session's layout -- two recessed 10-key keypads with the gold centre panel,
 * the LED and the CLEAR bar between them -- with Map / Clear / Defaults /
 * Show bindings and a row per gamepad. Hidden at start; a fixed-size
 * floating NSPanel.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "studio2session.h"

@interface Studio2ControllersWindow : NSObject
- (instancetype)initWithSession:(studio2session *)session;
/* Show, or hide when showing. */
- (void)toggle;
- (void)show;
- (void)hide;
- (BOOL)isVisible;
/* The settings changed (Settings > Reset Controls): bindings and pads are read
 * again. */
- (void)refresh;
@end
