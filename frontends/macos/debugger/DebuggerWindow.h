/*
 * The AppKit debugger window over the CDP1802 / CDP1861 debugger engine
 * (studio2debug.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "studio2session.h"

@interface Studio2DebuggerWindow : NSObject <NSWindowDelegate, NSTextFieldDelegate>
/* Shows (creating on first use) the debugger for the session; attaching the
 * engine stops the machine, as the other frontends do. */
+ (void)showForSession:(studio2session *)session;
/* F12: shows it, or closes it (detaching, so the machine runs on). */
+ (void)toggleForSession:(studio2session *)session;
@end
