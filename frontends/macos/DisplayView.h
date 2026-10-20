/*
 * The emulator display.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "studio2session.h"

@interface Studio2DisplayView : NSView
- (instancetype)initWithSession:(studio2session *)session;
/* YES: the 100x246 picture at the television's 4:3 (setting "aspect" 0, the
 * default); NO: square pixels, a very tall image. */
- (void)setTvAspect:(BOOL)tv;
- (void)setSmooth:(BOOL)smooth;
- (void)stop;
@end
