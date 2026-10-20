/*
 * The keypad panel: the Studio II's two 10-key keypads side by side, the
 * console's CLEAR button and the session's actions, the connected gamepads
 * and the Map row, in a fixed-size floating panel.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "studio2session.h"

@interface Studio2KeypadWindow : NSWindowController <NSWindowDelegate>
+ (void)toggleWithSession:(studio2session *)session;
+ (BOOL)isVisible;
@end
