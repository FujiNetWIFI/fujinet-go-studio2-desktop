/*
 * Studio2ControllersWindow -- the AppKit Controllers panel: the RCA Studio
 * II's top drawn from the session's layout (studio2session_controls_layout)
 * with NSBezierPath, so every frontend shows the same console; the gamepads
 * and the keypad each one plays; and Map mode.
 *
 * The console is a khaki-tan case with a recessed well in each wing (a black
 * bezel round a brushed-aluminium plate) holding a 3x3 keypad of black keys
 * with the 0 centred under the 8, A on the left and B on the right. Between
 * them is the gold centre panel (the RCA badge, the script "Studio II", HOME
 * TV PROGRAMMER), a ribbed panel with a silver plate holding the red LED and
 * the black CLEAR bar, and the session's CONFIG and STOP buttons. Colours are
 * DESIGN-studio2.md's; a held key is the accent (the case tan) with BLACK ink
 * and an outline in the dark accent.
 *
 * The pointer is its own input source: a press holds a key or CLEAR until the
 * button is released or the pointer drags off, and the panel lets go of
 * everything the mouse holds when it loses key status, is hidden or enters
 * Map (a CLEAR left down holds the CPU in reset, which freezes the machine).
 * CONFIG and STOP fire on release over the button, like real ones. What the
 * machine sees held -- host keyboard, gamepads and the mouse -- is painted in
 * the accent, so the panel doubles as an input tester for a new gamepad. Keys
 * typed with the panel key go to the machine as in the main window.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents.
 * Hidden at start, ordered out rather than closed, so its position survives.
 * The 30 Hz poll runs only while the panel can be seen.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control (all 23 targets are
 * on the picture), then press a key or a gamepad button; the next one is
 * bound and what held it is told in the status line. Clear unbinds the pick;
 * Esc cancels the pick; the unpicked controls are drawn at 40%. F9, F11 and
 * F12 are the app's and are never bound.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../KeyForward.h"
#import "../help/HelpWindow.h"

#include <math.h>
#include <string.h>

#define UNIT         32.0    /* points per layout unit (a key's height) */
#define CASE_PAD     16.0    /* the case round the controls */
#define MAX_CTL      64      /* room for the core's 38 controls */
#define MARGIN       16.0
#define TITLE_H      18.0
#define ROW_H        24.0
#define EFF_W       120.0
#define POPUP_W     108.0
#define BTN_H        28.0
#define MIN_PAD_ROWS 2       /* rows kept for pads, more when more are connected */
#define MAX_PAD_ROWS 4
#define SLOW_TICKS   8       /* the 30 Hz tick's 250 ms part */
#define DIM_ALPHA    0.4     /* controls that are not the Map pick */

/* ---- colours (DESIGN-studio2.md: photo-read where the photos agree, else
 * inferred) ------------------------------------------------------------------ */

#define COL_CASE_TOP    0xCDBB93     /* khaki-tan, around the accent */
#define COL_CASE_BOT    0xC0AC81
#define COL_CASE_EDGE   0xA8946A
#define COL_BAND        0x6E6648     /* the darker underside band */
#define COL_CAPTION     0xA8946A     /* the raised A and B in the case plastic */
#define COL_BEZEL       0x1C1A16
#define COL_PLATE       0xC0C1BB     /* brushed aluminium */
#define COL_BRUSH       0xB0B1AB
#define COL_KEY         0x2A2822
#define COL_KEY_HOT     0x3A3832
#define COL_KEY_INK     0xF2F2EC
#define COL_OUTLINE     0x7A6652     /* outlines, and a held key's outline */
#define COL_GOLD        0xC4B58C
#define COL_RIBBED      0xB3AC7C
#define COL_SILVER      0xCDCEC1
#define COL_CHROME      0xDADAD2
#define COL_LEGEND      0x1A1814
#define COL_LED_OFF     0x8E1E1A
#define COL_LED_ON      0xE8372E

static NSString *nsstr(const char *text)
{
    NSString *res = text ? [NSString stringWithUTF8String:text] : nil;
    return res ? res : @"";
}

static NSColor *rgb(uint32_t value)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:1.0];
}

/* The accent: the case tan, with black text on it. */
static NSColor *accent(void) { return Studio2AccentColor(); }

/* What is bound to a target ("Q / A", "" when nothing). */
static NSString *bindingText(studio2session *session, int target)
{
    char text[96];
    text[0] = '\0';
    if (target >= 0) studio2session_binding_text(session, target, text, (int)sizeof text);
    return nsstr(text);
}

/* Toggled-on look for a push button: accent bezel, black title. */
static void setButtonAccent(NSButton *button, BOOL on)
{
    if (!button) return;
    NSString *title = [button.title copy];
    if (on) {
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentCenter;
        button.bezelColor = accent();
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:title
                attributes:@{
                    NSFontAttributeName : button.font ?: [NSFont systemFontOfSize:NSFont.systemFontSize],
                    NSForegroundColorAttributeName : NSColor.blackColor,
                    NSParagraphStyleAttributeName : para
                }];
    } else {
        button.bezelColor = nil;
        button.title = title;
    }
}

/* A vertical gradient clipped to a path (view coordinates: the views here are
 * flipped, so "top" is the smaller y). */
static void fillGradient(NSBezierPath *path, uint32_t topRgb, uint32_t botRgb)
{
    const NSRect bnd = [path bounds];
    const CGFloat comps[8] = {
        ((topRgb >> 16) & 0xff) / 255.0, ((topRgb >> 8) & 0xff) / 255.0, (topRgb & 0xff) / 255.0, 1.0,
        ((botRgb >> 16) & 0xff) / 255.0, ((botRgb >> 8) & 0xff) / 255.0, (botRgb & 0xff) / 255.0, 1.0 };
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef grad = CGGradientCreateWithColorComponents(space, comps, NULL, 2);
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    CGContextDrawLinearGradient(ctx, grad, CGPointMake(NSMinX(bnd), NSMinY(bnd)),
                                CGPointMake(NSMinX(bnd), NSMaxY(bnd)), 0);
    [NSGraphicsContext restoreGraphicsState];
    CGGradientRelease(grad);
    CGColorSpaceRelease(space);
}

/* A small direction triangle at mid, pointing along (dx, dy). */
static void drawMark(NSPoint mid, CGFloat size, CGFloat dx, CGFloat dy, NSColor *ink)
{
    const CGFloat sx = -dy, sy = dx;
    NSBezierPath *tri = [NSBezierPath bezierPath];
    [tri moveToPoint:NSMakePoint(mid.x + dx * size, mid.y + dy * size)];
    [tri lineToPoint:NSMakePoint(mid.x - dx * size * 0.6 + sx * size * 0.8,
                                 mid.y - dy * size * 0.6 + sy * size * 0.8)];
    [tri lineToPoint:NSMakePoint(mid.x - dx * size * 0.6 - sx * size * 0.8,
                                 mid.y - dy * size * 0.6 - sy * size * 0.8)];
    [tri closePath];
    [ink setFill];
    [tri fill];
}

/* One line of text centred in rect, the font shrunk (to 5 pt) until it fits. */
static void drawFitted(NSString *text, NSRect rect, CGFloat size, BOOL bold, NSColor *ink)
{
    if (!text.length) return;
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    NSSize extent = NSZeroSize;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        attrs = @{ NSFontAttributeName: font,
                   NSParagraphStyleAttributeName: para,
                   NSForegroundColorAttributeName: ink };
        extent = [text sizeWithAttributes:attrs];
        if ((extent.width <= rect.size.width && extent.height <= rect.size.height + 2) || size <= 5) break;
        size -= 0.5;
    }
    const CGFloat ypos = rect.origin.y + floor((rect.size.height - extent.height) / 2);
    [text drawInRect:NSMakeRect(rect.origin.x, ypos, rect.size.width, extent.height) withAttributes:attrs];
}

/* ---- views --------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' panels. */
@interface Studio2FlippedView : NSView
@end

@implementation Studio2FlippedView
- (BOOL)isFlipped { return YES; }
@end

/* Keys typed with the panel key go to its owner. */
@class Studio2ControllersWindow;
@interface Studio2ControllersPanel : NSPanel
@property (nonatomic, weak) Studio2ControllersWindow *owner;
@end

@interface Studio2ControllersWindow () <NSWindowDelegate>
- (void)mapPick:(int)target;
- (NSString *)legendForTarget:(int)target;
- (void)runSysaction:(int)sysact;
- (void)panelKeyDown:(NSEvent *)evt;
- (void)panelKeyUp:(NSEvent *)evt;
- (void)panelFlagsChanged:(NSEvent *)evt;
@end

/* The console: the case, the two keypad wells, the centre panel and the
 * session buttons. Everything on it is drawn here; the layout and the exact
 * hit rectangles are the core's. */
@interface Studio2ConsoleView : Studio2FlippedView
@property (nonatomic) studio2session *session;
@property (nonatomic, weak) Studio2ControllersWindow *owner;
@property (nonatomic) BOOL mapping;          /* Map armed: bindings shown, nothing lit */
@property (nonatomic) int mapTarget;         /* the control waiting for a key, or -1 */
@property (nonatomic) BOOL showKeys;         /* the bound host key as a small legend */
@property (nonatomic, readonly) NSSize contentSize;
- (void)configure;
/* Reads what the machine sees held; redraws when anything changed. */
- (void)poll;
/* Lets go of everything the pointer holds. */
- (void)letGo;
@end

@implementation Studio2ConsoleView {
    const studio2_control *_ctl;
    int _num;
    float _layW, _layH;
    NSSize _size;
    BOOL _lit[MAX_CTL];
    int _hover;                  /* the control under the pointer, or -1 */
    int _downIdx;                /* the control pressed, or -1 */
    BOOL _sysDown;               /* the pressed control is a session button */
    NSTrackingArea *_tracking;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _downIdx = -1;
    _hover = -1;
    _mapTarget = -1;
    return self;
}

- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)evt { (void)evt; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (NSSize)contentSize { return _size; }

- (void)configure
{
    _ctl = studio2session_controls_layout(&_num, &_layW, &_layH);
    if (!_ctl) _num = 0;
    if (_num > MAX_CTL) _num = MAX_CTL;
    _size = NSMakeSize(ceil(_layW * UNIT) + 2 * CASE_PAD, ceil(_layH * UNIT) + 2 * CASE_PAD);
    memset(_lit, 0, sizeof _lit);
    [self setNeedsDisplay:YES];
}

- (void)updateTrackingAreas
{
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                      NSTrackingActiveAlways | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

/* ---- geometry ------------------------------------------------------------------ */

- (NSRect)rectOf:(const studio2_control *)ctl
{
    return NSMakeRect(CASE_PAD + ctl->xpos * UNIT, CASE_PAD + ctl->ypos * UNIT,
                      ctl->width * UNIT, ctl->height * UNIT);
}

/* The pressable control under a view point, or -1. */
- (int)hitAt:(NSPoint)pnt
{
    const int idx = studio2session_controls_hit((float)((pnt.x - CASE_PAD) / UNIT),
                                                (float)((pnt.y - CASE_PAD) / UNIT));
    return idx >= 0 && idx < _num ? idx : -1;
}

/* ---- the held state --------------------------------------------------------------- */

- (void)poll
{
    BOOL dirty = NO;
    for (int idx = 0; idx < _num; idx++) {
        const studio2_control *ctl = &_ctl[idx];
        BOOL lit = NO;
        switch (ctl->kind) {
        case STUDIO2_CTL_KEY:
        case STUDIO2_CTL_CLEAR:
        case STUDIO2_CTL_LED:
            lit = !self.mapping && ctl->target >= 0 && studio2session_target_held(_session, ctl->target) != 0;
            break;
        case STUDIO2_CTL_SYSBUTTON:
            lit = !self.mapping && _sysDown && idx == _downIdx;
            break;
        default:
            break;
        }
        if (lit != _lit[idx]) {
            _lit[idx] = lit;
            dirty = YES;
        }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

/* ---- drawing -------------------------------------------------------------------------- */

static BOOL pressable(const studio2_control *ctl)
{
    return ctl->kind == STUDIO2_CTL_KEY || ctl->kind == STUDIO2_CTL_CLEAR ||
           ctl->kind == STUDIO2_CTL_SYSBUTTON;
}

- (BOOL)selected:(const studio2_control *)ctl
{
    return self.mapping && ctl->target >= 0 && ctl->target == self.mapTarget;
}

/* Whether a control is drawn faint: in Map mode everything pressable but the
 * pick (or, while nothing is picked yet, the one under the pointer). The LED
 * follows CLEAR. */
- (BOOL)dimmed:(int)idx
{
    if (!self.mapping) return NO;
    const studio2_control *ctl = &_ctl[idx];
    if (!pressable(ctl) && ctl->kind != STUDIO2_CTL_LED) return NO;
    if ([self selected:ctl]) return NO;
    return !(self.mapTarget < 0 && idx == _hover);
}

- (void)beginDim:(BOOL)dim
{
    if (!dim) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextSaveGState(ctx);
    CGContextSetAlpha(ctx, DIM_ALPHA);
    CGContextBeginTransparencyLayer(ctx, NULL);
}

- (void)endDim:(BOOL)dim
{
    if (!dim) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextEndTransparencyLayer(ctx);
    CGContextRestoreGState(ctx);
}

/* The keypad well: black bezel, brushed-aluminium plate inside. */
- (void)drawWell:(const studio2_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    NSBezierPath *bezel = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:6 yRadius:6];
    [rgb(COL_BEZEL) setFill];
    [bezel fill];
    const NSRect plate = NSInsetRect(rect, 5, 5);
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:plate xRadius:3 yRadius:3];
    [rgb(COL_PLATE) setFill];
    [path fill];
    /* the brushing: hairlines along the plate */
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    [rgb(COL_BRUSH) setStroke];
    for (CGFloat ypos = NSMinY(plate) + 3; ypos < NSMaxY(plate); ypos += 4) {
        NSBezierPath *line = [NSBezierPath bezierPath];
        line.lineWidth = 0.5;
        [line moveToPoint:NSMakePoint(NSMinX(plate), ypos)];
        [line lineToPoint:NSMakePoint(NSMaxX(plate), ypos)];
        [line stroke];
    }
    [NSGraphicsContext restoreGraphicsState];

    /* the direction marks round the grid: 2 4 6 8 and the diagonals */
    NSRect keyRect[10];
    BOOL have[10] = {NO};
    for (int idx = 0; idx < _num; idx++) {
        if (_ctl[idx].kind != STUDIO2_CTL_KEY || _ctl[idx].port != ctl->port) continue;
        const int key = _ctl[idx].target % STUDIO2_ACT_PER_PORT;
        keyRect[key] = [self rectOf:&_ctl[idx]];
        have[key] = YES;
    }
    for (int key = 0; key < 10; key++)
        if (!have[key]) return;
    const CGFloat sz = 3.4, dg = 0.7071;
    NSColor *mark = rgb(0x62635E);
    drawMark(NSMakePoint(NSMidX(keyRect[2]), NSMinY(keyRect[2]) - 7), sz, 0, -1, mark);
    drawMark(NSMakePoint(NSMinX(keyRect[4]) - 7, NSMidY(keyRect[4])), sz, -1, 0, mark);
    drawMark(NSMakePoint(NSMaxX(keyRect[6]) + 7, NSMidY(keyRect[6])), sz, 1, 0, mark);
    drawMark(NSMakePoint(NSMidX(keyRect[8]), (NSMaxY(keyRect[8]) + NSMinY(keyRect[0])) / 2), sz, 0, 1, mark);
    drawMark(NSMakePoint(NSMinX(keyRect[1]) - 7, NSMinY(keyRect[1]) - 7), sz * 0.85, -dg, -dg, mark);
    drawMark(NSMakePoint(NSMaxX(keyRect[3]) + 7, NSMinY(keyRect[3]) - 7), sz * 0.85, dg, -dg, mark);
    drawMark(NSMakePoint(NSMinX(keyRect[7]) - 7, NSMaxY(keyRect[7]) + 7), sz * 0.85, -dg, dg, mark);
    drawMark(NSMakePoint(NSMaxX(keyRect[9]) + 7, NSMaxY(keyRect[9]) + 7), sz * 0.85, dg, dg, mark);
}

/* The centre panel's plates: gold, ribbed, silver. */
- (void)drawPanel:(const studio2_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const char *label = ctl->label ? ctl->label : "";
    if (!strcmp(label, "gold")) {
        /* the chrome trim frames the whole centre panel: drawn with the first plate */
        NSBezierPath *trim = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -2, -2)
                                                             xRadius:4 yRadius:4];
        [rgb(COL_CHROME) setFill];
        [trim fill];
        NSBezierPath *path = [NSBezierPath bezierPathWithRect:rect];
        [rgb(COL_GOLD) setFill];
        [path fill];
    } else if (!strcmp(label, "ribbed")) {
        NSBezierPath *trim = [NSBezierPath bezierPathWithRect:NSInsetRect(rect, -2, 0)];
        [rgb(COL_CHROME) setFill];
        [trim fill];
        NSBezierPath *path = [NSBezierPath bezierPathWithRect:rect];
        [rgb(COL_RIBBED) setFill];
        [path fill];
        /* the grooves */
        [rgb(COL_OUTLINE) setStroke];
        for (CGFloat ypos = NSMinY(rect) + 4; ypos < NSMaxY(rect) - 1; ypos += 5) {
            NSBezierPath *line = [NSBezierPath bezierPath];
            line.lineWidth = 0.7;
            [line moveToPoint:NSMakePoint(NSMinX(rect), ypos)];
            [line lineToPoint:NSMakePoint(NSMaxX(rect), ypos)];
            [line stroke];
        }
    } else {
        NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:2 yRadius:2];
        [rgb(COL_SILVER) setFill];
        [path fill];
        [rgb(COL_OUTLINE) setStroke];
        path.lineWidth = 1;
        [path stroke];
    }
}

/* Text on the console: the A and B over the wells, the RCA badge, the script
 * name, the tagline and the word under the CLEAR bar. */
- (void)drawCaption:(const studio2_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    NSString *text = nsstr(ctl->label);
    if (ctl->port >= 0) {
        drawFitted(text, rect, 20, YES, rgb(COL_CAPTION));
    } else if ([text isEqualToString:@"RCA"]) {
        /* the white-and-tan square badge */
        NSBezierPath *badge = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:2 yRadius:2];
        [[NSColor whiteColor] setFill];
        [badge fill];
        [rgb(COL_OUTLINE) setStroke];
        badge.lineWidth = 1;
        [badge stroke];
        drawFitted(text, NSInsetRect(rect, 2, 2), 11, YES, rgb(COL_LEGEND));
    } else if ([text isEqualToString:@"Studio II"]) {
        NSFont *script = [NSFont fontWithName:@"Snell Roundhand" size:22];
        if (!script) script = [NSFont fontWithName:@"Zapfino" size:16];
        if (!script) script = [[NSFontManager sharedFontManager]
                                  convertFont:[NSFont systemFontOfSize:20] toHaveTrait:NSItalicFontMask];
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentLeft;
        NSDictionary *attrs = @{ NSFontAttributeName: script,
                                 NSParagraphStyleAttributeName: para,
                                 NSForegroundColorAttributeName: rgb(COL_LEGEND) };
        const NSSize extent = [text sizeWithAttributes:attrs];
        [text drawInRect:NSMakeRect(rect.origin.x, rect.origin.y + floor((rect.size.height - extent.height) / 2),
                                    rect.size.width, extent.height)
          withAttributes:attrs];
    } else {
        drawFitted(text, rect, 8, YES, rgb(COL_LEGEND));
    }
}

- (void)drawLed:(const studio2_control *)ctl lit:(BOOL)lit
{
    const NSRect rect = [self rectOf:ctl];
    NSBezierPath *ring = [NSBezierPath bezierPathWithOvalInRect:rect];
    fillGradient(ring, 0xEFEFEA, 0x8E8F8A);          /* the chrome bezel */
    NSBezierPath *lens = [NSBezierPath bezierPathWithOvalInRect:NSInsetRect(rect, 3, 3)];
    [rgb(lit ? COL_LED_ON : COL_LED_OFF) setFill];
    [lens fill];
    if (lit) {
        [[NSColor colorWithSRGBRed:1.0 green:0.78 blue:0.74 alpha:0.9] setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(NSMinX(rect) + 6, NSMinY(rect) + 5, 4, 3)] fill];
    }
}

/* A keypad key, the CLEAR bar or a session button: black, or the accent with
 * black ink when held (or picked in Map). */
- (void)drawKey:(const studio2_control *)ctl idx:(int)idx lit:(BOOL)lit
{
    const BOOL held = lit || [self selected:ctl];
    const BOOL showing = self.mapping || self.showKeys;
    const BOOL sys = ctl->kind == STUDIO2_CTL_SYSBUTTON;
    const BOOL key = ctl->kind == STUDIO2_CTL_KEY;
    const NSRect rect = NSInsetRect([self rectOf:ctl], 0.5, 0.5);
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:sys ? 3 : 4 yRadius:sys ? 3 : 4];
    if (held) {
        [accent() setFill];
        [path fill];
        [rgb(COL_OUTLINE) setStroke];
        path.lineWidth = 2;
        [path stroke];
    } else {
        fillGradient(path, idx == _hover && !self.mapping ? COL_KEY_HOT : COL_KEY, 0x1E1C18);
        [rgb(COL_OUTLINE) setStroke];
        path.lineWidth = 1;
        [path stroke];
    }
    NSColor *ink = held ? [NSColor blackColor] : rgb(COL_KEY_INK);
    NSString *face = nsstr(ctl->label);
    const NSRect inner = NSInsetRect(rect, 3, 2);
    if (showing && ctl->target >= 0) {
        NSString *bound = bindingText(_session, ctl->target);
        if (key) {
            /* the digit small in the corner, the binding on the face */
            drawFitted(face, NSMakeRect(inner.origin.x, inner.origin.y - 1, 12, 10), 8, YES,
                       held ? [NSColor blackColor] : rgb(0xA8A89F));
            drawFitted(bound, NSMakeRect(inner.origin.x, inner.origin.y + 5, inner.size.width,
                                         inner.size.height - 5), 9, NO, ink);
        } else {
            drawFitted(bound.length ? bound : face, inner, bound.length ? 8 : 10, sys, ink);
        }
    } else {
        drawFitted(face, inner, key ? 15 : 9, YES, ink);
    }
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect bnd = [self bounds];

    /* the case, with its darker underside band */
    NSBezierPath *frame = [NSBezierPath bezierPathWithRect:bnd];
    fillGradient(frame, COL_CASE_TOP, COL_CASE_BOT);
    [rgb(COL_BAND) setFill];
    NSRectFill(NSMakeRect(0, NSMaxY(bnd) - 6, bnd.size.width, 6));
    NSBezierPath *edge = [NSBezierPath bezierPathWithRect:NSInsetRect(bnd, 0.5, 0.5)];
    [rgb(COL_CASE_EDGE) setStroke];
    edge.lineWidth = 1;
    [edge stroke];

    /* the parts, in the layout's paint order */
    for (int idx = 0; idx < _num; idx++) {
        const studio2_control *ctl = &_ctl[idx];
        const BOOL dim = [self dimmed:idx];
        [self beginDim:dim];
        switch (ctl->kind) {
        case STUDIO2_CTL_WELL:      [self drawWell:ctl]; break;
        case STUDIO2_CTL_PANEL:     [self drawPanel:ctl]; break;
        case STUDIO2_CTL_CAPTION:   [self drawCaption:ctl]; break;
        case STUDIO2_CTL_LED:       [self drawLed:ctl lit:_lit[idx]]; break;
        case STUDIO2_CTL_KEY:
        case STUDIO2_CTL_CLEAR:
        case STUDIO2_CTL_SYSBUTTON: [self drawKey:ctl idx:idx lit:_lit[idx]]; break;
        default: break;
        }
        [self endDim:dim];
    }

    /* Map mode: the picked control is called out */
    if (self.mapping && self.mapTarget >= 0) {
        for (int idx = 0; idx < _num; idx++) {
            const studio2_control *ctl = &_ctl[idx];
            if (!pressable(ctl) || ctl->target != self.mapTarget) continue;
            NSBezierPath *ring = [NSBezierPath bezierPathWithRect:NSInsetRect([self rectOf:ctl], -2, -2)];
            ring.lineWidth = 2;
            [accent() setStroke];
            [ring stroke];
        }
    }
}

/* ---- the mouse ---------------------------------------------------------------------- */

- (void)letGo
{
    studio2session_release_pointer(_session);
    _downIdx = -1;
    _sysDown = NO;
    [self setNeedsDisplay:YES];
}

- (void)mouseMoved:(NSEvent *)evt
{
    const int hover = [self hitAt:[self convertPoint:[evt locationInWindow] fromView:nil]];
    if (hover != _hover) {
        _hover = hover;
        [self setNeedsDisplay:YES];
    }
}

- (void)mouseExited:(NSEvent *)evt
{
    (void)evt;
    if (_hover >= 0) {
        _hover = -1;
        [self setNeedsDisplay:YES];
    }
}

- (void)mouseDown:(NSEvent *)evt
{
    if (_downIdx >= 0) return;
    const int idx = [self hitAt:[self convertPoint:[evt locationInWindow] fromView:nil]];
    if (idx < 0) return;
    const studio2_control *ctl = &_ctl[idx];

    if (self.mapping) {
        if (ctl->target >= 0) [self.owner mapPick:ctl->target];
        return;
    }
    _downIdx = idx;
    if (ctl->kind == STUDIO2_CTL_SYSBUTTON)
        _sysDown = YES;      /* fires on release over the button */
    else
        studio2session_pointer_press(_session, ctl->target, 1);
    [self setNeedsDisplay:YES];
    [self poll];
}

- (void)mouseDragged:(NSEvent *)evt
{
    if (_downIdx < 0) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];
    /* a key or CLEAR: dragging off lets go */
    if (!_sysDown && !NSPointInRect(pnt, [self rectOf:&_ctl[_downIdx]])) {
        [self letGo];
        [self poll];
    }
}

- (void)mouseUp:(NSEvent *)evt
{
    if (_downIdx < 0) return;
    if (_sysDown) {
        /* a session button acts on release over it, like a real one */
        const studio2_control *ctl = &_ctl[_downIdx];
        const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];
        const BOOL inside = NSPointInRect(pnt, [self rectOf:ctl]);
        const int target = ctl->target;
        [self letGo];
        [self poll];
        if (inside && target >= STUDIO2_TARGET_SYSACT(0) && target < STUDIO2_TARGET_COUNT)
            [self.owner runSysaction:target - STUDIO2_TARGET_SYSACT(0)];
        return;
    }
    [self letGo];
    [self poll];
}

/* The tooltip: the target's name and what is bound to it. */
- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)pnt userData:(void *)data
{
    (void)view;
    (void)tag;
    (void)data;
    const int idx = [self hitAt:pnt];
    if (idx < 0 || _ctl[idx].target < 0) return nil;
    NSString *bound = bindingText(_session, _ctl[idx].target);
    return [NSString stringWithFormat:@"%s\n%@", studio2_target_name(_ctl[idx].target),
            bound.length ? bound : @"not bound"];
}

/* One tooltip rect over the whole console; the string is made per point. */
- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self removeAllToolTips];
    [self addToolTipRect:[self bounds] owner:self userData:NULL];
}
@end

@implementation Studio2ControllersPanel
- (void)keyDown:(NSEvent *)evt { [self.owner panelKeyDown:evt]; }
- (void)keyUp:(NSEvent *)evt { [self.owner panelKeyUp:evt]; }
- (void)flagsChanged:(NSEvent *)evt { [self.owner panelFlagsChanged:evt]; }
/* Escape cancels a Map pick; it is not "close the panel". */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------- */

@implementation Studio2ControllersWindow {
    studio2session *_session;
    Studio2ControllersPanel *_panel;
    Studio2FlippedView *_content;
    Studio2ConsoleView *_console;
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSTextField *_padEff[MAX_PAD_ROWS];     /* each pad's "plays keypad A" */
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    int _padRows;                           /* the rows showing a pad */
    int _reservedRows;                      /* the rows the panel has room for */
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_clearButton;
    NSButton *_defaultsButton;
    NSButton *_showKeysBox;
    NSButton *_helpButton;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    /* -2 idle, -1 armed and waiting for a control, >= 0 waiting for a key or
     * gamepad button to bind to that target */
    int _mapState;
    BOOL _placed;
}

- (instancetype)initWithSession:(studio2session *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _mapState = -2;
    _padGeneration = (unsigned)-1;
    _reservedRows = MIN_PAD_ROWS;

    _panel = [[Studio2ControllersPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 500, 500)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers"];
    /* Floats above the machine's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    _panel.owner = self;
    [_panel setDelegate:self];
    STUDIO2SetHelpContext(_panel, @"controllers");

    _content = [[Studio2FlippedView alloc] initWithFrame:NSMakeRect(0, 0, 500, 500)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];

    _console = [[Studio2ConsoleView alloc] initWithFrame:NSZeroRect];
    _console.owner = self;
    _console.session = _session;
    _console.mapTarget = -1;
    _console.showKeys = studio2session_get_int(_session, "controllers_show_bindings", 0) != 0;
    [_content addSubview:_console];

    /* Gamepads: one row per pad, its name, the keypad it plays and the one it
     * is set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads connected - plug one in"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_content addSubview:_noPads];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        _padName[row] = [NSTextField labelWithString:@""];
        [_padName[row] setFont:[NSFont systemFontOfSize:11]];
        [_padName[row] setLineBreakMode:NSLineBreakByTruncatingTail];
        [_content addSubview:_padName[row]];
        _padEff[row] = [NSTextField labelWithString:@""];
        [_padEff[row] setFont:[NSFont systemFontOfSize:11]];
        [_padEff[row] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[row] setAlignment:NSTextAlignmentRight];
        [_content addSubview:_padEff[row]];
        /* -1 = automatic (connection order), 0 = keypad A, 1 = keypad B */
        _padAssign[row] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        [_padAssign[row] addItemsWithTitles:@[@"Automatic", @"Keypad A", @"Keypad B"]];
        _padAssign[row].tag = row;
        _padAssign[row].target = self;
        _padAssign[row].action = @selector(padAssignChanged:);
        [_padAssign[row] setRefusesFirstResponder:YES];
        [_content addSubview:_padAssign[row]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [_content addSubview:_mapButton];
    _clearButton = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearBinding:)];
    [_clearButton setRefusesFirstResponder:YES];
    [_content addSubview:_clearButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [_content addSubview:_defaultsButton];
    _showKeysBox = [NSButton checkboxWithTitle:@"Show bindings" target:self action:@selector(toggleShowKeys:)];
    [_showKeysBox setState:_console.showKeys ? NSControlStateValueOn : NSControlStateValueOff];
    [_showKeysBox setRefusesFirstResponder:YES];
    [_content addSubview:_showKeysBox];
    _helpButton = STUDIO2HelpButton(@"controllers");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];
    _hint = [NSTextField wrappingLabelWithString:@""];
    [_hint setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setMaximumNumberOfLines:2];
    [[_hint cell] setTruncatesLastVisibleLine:YES];
    [_content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_panel];

    [_console configure];
    [self setMapState:-2];
    [self relayout];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- the console ------------------------------------------------------------------- */

/* A new content size with the top-left corner where it was. */
- (void)fitPanel:(NSSize)size
{
    const NSRect frame = [_panel frame];
    NSRect content = [_panel contentRectForFrameRect:frame];
    if (NSEqualSizes(content.size, size)) return;
    content.size = size;
    NSRect next = [_panel frameRectForContentRect:content];
    next.origin.x = frame.origin.x;
    next.origin.y = NSMaxY(frame) - next.size.height;
    [_panel setFrame:next display:YES];
}

- (void)relayout
{
    const NSSize csize = _console.contentSize;
    const CGFloat wide = csize.width;
    const CGFloat inner = wide - 2 * MARGIN;
    const CGFloat padsY = csize.height + 10;
    const CGFloat rowsY = padsY + TITLE_H;
    const CGFloat mapY = rowsY + _reservedRows * ROW_H + 6;
    const CGFloat hintY = mapY + BTN_H + 6;
    const CGFloat total = hintY + 30 + MARGIN;

    [self fitPanel:NSMakeSize(wide, total)];
    [_console setFrame:NSMakeRect(0, 0, csize.width, csize.height)];
    [_padsTitle setFrame:NSMakeRect(MARGIN, padsY, inner, 16)];
    [_noPads setFrame:NSMakeRect(MARGIN, rowsY + 3, inner, 18)];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const CGFloat ypos = rowsY + row * ROW_H;
        [_padName[row] setFrame:NSMakeRect(MARGIN, ypos + 4, inner - POPUP_W - EFF_W - 12, 16)];
        [_padEff[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W - EFF_W - 6, ypos + 4, EFF_W, 16)];
        [_padAssign[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W, ypos - 1, POPUP_W, 25)];
    }
    [_mapButton setFrame:NSMakeRect(MARGIN, mapY, 56, BTN_H)];
    [_clearButton setFrame:NSMakeRect(MARGIN + 62, mapY, 56, BTN_H)];
    [_defaultsButton setFrame:NSMakeRect(MARGIN + 124, mapY, 72, BTN_H)];
    [_showKeysBox setFrame:NSMakeRect(MARGIN + 206, mapY + 3, 122, 22)];
    [_helpButton setFrameOrigin:NSMakePoint(wide - MARGIN - _helpButton.frame.size.width, mapY + 2)];
    [_hint setFrame:NSMakeRect(MARGIN, hintY, inner, 30)];
    [_console setNeedsDisplay:YES];
}

/* Lets go of what the mouse holds in this panel. */
- (void)letGo
{
    [_console letGo];
}

/* ---- Map mode --------------------------------------------------------------------- */

- (NSString *)legendForTarget:(int)target
{
    return bindingText(_session, target);
}

/* In Map mode nothing is lit and every control shows what is bound to it. */
- (void)refreshLabels
{
    _console.mapping = _mapState != -2;
    _console.mapTarget = _mapState >= 0 ? _mapState : -1;
    [_console setNeedsDisplay:YES];
    [_console poll];
}

- (void)setMapState:(int)state
{
    _mapState = state;
    if (state != -2) {
        /* a pointer or keyboard hold must not outlive the mode that ignores
         * its release: a CLEAR left down freezes the machine */
        [self letGo];
        studio2session_release_all(_session);
    }
    if (state >= 0) {
        studio2session_gamepad_capture_begin(_session);
        _hint.stringValue = [NSString stringWithFormat:@"Press a key or a gamepad button for %s (now: %@). Esc cancels.",
                             studio2_target_name(state), [self bindingOrNone:state]];
    } else {
        studio2session_gamepad_capture_cancel(_session);
        _hint.stringValue = state == -1
            ? @"Click a control to remap it."
            : @"Click and hold the keys and CLEAR; CONFIG and STOP act on release. Keys and gamepads work here too.";
    }
    [_mapButton setTitle:(state == -2 ? @"Map" : @"Done")];
    setButtonAccent(_mapButton, state != -2);
    _clearButton.enabled = state >= 0;
    [self refreshLabels];
}

- (NSString *)bindingOrNone:(int)target
{
    NSString *text = bindingText(_session, target);
    return text.length ? text : @"not bound";
}

/* A control clicked on the picture in Map mode. */
- (void)mapPick:(int)target
{
    if (_mapState != -2) [self setMapState:target];
}

/* "Bound X to <target> (was <previous holder>)". */
- (void)showBound:(NSString *)name target:(int)target stolen:(const char *)stolen
{
    _hint.stringValue = stolen && stolen[0]
        ? [NSString stringWithFormat:@"Bound %@ to %s (was %s)", name, studio2_target_name(target), stolen]
        : [NSString stringWithFormat:@"Bound %@ to %s", name, studio2_target_name(target)];
}

- (void)bindKey:(uint32_t)keysym
{
    const int target = _mapState;
    char stolen[128], name[48];
    stolen[0] = '\0';
    studio2session_binding_set_key(_session, target, keysym, stolen, sizeof stolen);
    studio2session_keysym_name(keysym, name, sizeof name);
    [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
    [self showBound:nsstr(name) target:target stolen:stolen];
}

- (void)pollCapture
{
    int button;
    if (_mapState < 0 || !studio2session_gamepad_capture_poll(_session, &button)) return;
    char stolen[128];
    stolen[0] = '\0';
    const int target = _mapState;
    studio2session_binding_set_button(_session, target, button, stolen, sizeof stolen);
    [self setMapState:-1];
    [self showBound:nsstr(studio2_pad_button_name(button)) target:target stolen:stolen];
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapState:(_mapState == -2 ? -1 : -2)];
}

/* Clear: the picked control loses its key and its gamepad button. */
- (void)clearBinding:(id)sender
{
    (void)sender;
    if (_mapState < 0) return;
    const int target = _mapState;
    studio2session_binding_clear(_session, target);
    [self setMapState:-1];
    _hint.stringValue = [NSString stringWithFormat:@"Cleared %s", studio2_target_name(target)];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    studio2session_bindings_reset(_session);
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

- (void)toggleShowKeys:(id)sender
{
    (void)sender;
    const BOOL show = [_showKeysBox state] == NSControlStateValueOn;
    studio2session_set_int(_session, "controllers_show_bindings", show ? 1 : 0);
    _console.showKeys = show;
    [_console setNeedsDisplay:YES];
}

/* A session button on the panel was released over itself: posted, so the
 * application runs it exactly as it runs a gamepad's. */
- (void)runSysaction:(int)sysact
{
    studio2session_sysaction_post(_session, sysact);
}

/* ---- gamepads ----------------------------------------------------------------------- */

- (void)refreshPadKeypads
{
    for (int row = 0; row < _padRows; row++) {
        const int port = studio2session_gamepad_effective_port(_session, row);
        NSString *text = port >= 0 ? [NSString stringWithFormat:@"plays keypad %c", 'A' + port]
                                   : @"plays nothing";
        if (![_padEff[row].stringValue isEqualToString:text]) _padEff[row].stringValue = text;
    }
}

- (void)refreshPads
{
    const int count = studio2session_gamepad_count(_session);
    _padGeneration = studio2session_gamepad_generation(_session);
    _padRows = count < MAX_PAD_ROWS ? (count > 0 ? count : 0) : MAX_PAD_ROWS;
    /* room for the pads there are (two rows to start with, more when more
     * pads are connected); the panel changes size only then */
    const int rooms = _padRows > MIN_PAD_ROWS ? _padRows : MIN_PAD_ROWS;
    if (rooms != _reservedRows) {
        _reservedRows = rooms;
        [self relayout];
    }
    _noPads.hidden = count > 0;
    _padsTitle.stringValue = count > MAX_PAD_ROWS
        ? [NSString stringWithFormat:@"Gamepads (%d of %d shown)", MAX_PAD_ROWS, count]
        : @"Gamepads";
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const BOOL shown = row < _padRows;
        _padName[row].hidden = !shown;
        _padEff[row].hidden = !shown;
        _padAssign[row].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        studio2session_gamepad_name(_session, row, name, (int)sizeof name);
        _padName[row].stringValue = nsstr(name);
        _padName[row].toolTip = nsstr(name);
        const int assign = studio2session_gamepad_assignment(_session, row);
        [_padAssign[row] selectItemAtIndex:(assign < 0 || assign > 1 ? 0 : assign + 1)];
    }
    [self refreshPadKeypads];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    studio2session_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadKeypads];
}

/* ---- the poll ----------------------------------------------------------------------- */

/* 30 Hz while the panel can be seen: what is held and the gamepad capture;
 * every 250 ms the hot-plugged pads and the auto-assignment. */
- (void)tick
{
    if (_mapState >= 0) [self pollCapture];
    if (++_slow >= SLOW_TICKS) {
        _slow = 0;
        if (studio2session_gamepad_generation(_session) != _padGeneration) [self refreshPads];
        else [self refreshPadKeypads];
    }
    [_console poll];
}

/* The timer runs only while the panel is on screen and not occluded or
 * hidden with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak Studio2ControllersWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *tmr) {
            (void)tmr;
            [weakSelf tick];
        }];
        _timer.tolerance = 0.005;
    } else if (!want && _timer) {
        [_timer invalidate];
        _timer = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimer];
}

/* ---- the keyboard ------------------------------------------------------------------- */

/* Keyboard here behaves exactly as in the main window, so typing drives the
 * machine whichever window is key -- except in Map mode, where the next key
 * pressed is the binding. F9 toggles the panel; F11 and F12 are the main
 * window's and are never bound. */
- (void)panelKeyDown:(NSEvent *)evt
{
    /* Command shortcuts are the menus'; AppKit sends no keyUp while Command
     * is down, so forwarding one would leave the machine holding the key. */
    if ([evt modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t keysym = Studio2KeysymFromEvent(evt);
    if (!keysym) return;
    if (keysym == STUDIO2_KEYSYM_F9) {
        if (![evt isARepeat]) [self toggle];
        return;
    }
    if (keysym == STUDIO2_KEYSYM_F11 || keysym == STUDIO2_KEYSYM_F12) {
        if (_mapState >= 0) _hint.stringValue = @"F9, F11 and F12 are the app's and cannot be bound.";
        return;
    }
    if ([evt isARepeat]) return;
    if (_mapState >= 0) {
        /* the next key is the binding; Esc backs out of the pick */
        if (keysym == STUDIO2_KEYSYM_ESCAPE) [self setMapState:-1];
        else [self bindKey:keysym];
        return;
    }
    if (_mapState == -1) return;    /* armed: typing does nothing */
    const int sa = studio2session_key_sysaction(_session, keysym);
    if (sa >= 0) { studio2session_sysaction_post(_session, sa); return; }
    studio2session_key(_session, keysym, 1);
}

- (void)panelKeyUp:(NSEvent *)evt
{
    if (_mapState != -2) return;
    const uint32_t keysym = Studio2KeysymFromEvent(evt);
    if (keysym) studio2session_key(_session, keysym, 0);
}

/* Modifiers bind too (Shift, Control and Option are keys like any other --
 * Left Alt is keypad A's 0 by default). */
- (void)panelFlagsChanged:(NSEvent *)evt
{
    int down = 0;
    const uint32_t keysym = Studio2KeysymFromFlagsChange(evt, &down);
    if (!keysym) return;
    if (_mapState >= 0) {
        if (down) [self bindKey:keysym];
        return;
    }
    if (_mapState == -2) studio2session_key(_session, keysym, down);
}

/* ---- the window --------------------------------------------------------------------- */

/* Hidden: Map disarmed, nothing held, no polling. */
- (void)quiesce
{
    [self setMapState:-2];
    [self letGo];
    studio2session_release_all(_session);
    [_timer invalidate];
    _timer = nil;
}

- (void)show
{
    [self refreshPads];
    [self setMapState:-2];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
    [self updateTimer];
}

- (void)hide
{
    [_panel orderOut:nil];
    [self quiesce];
}

- (void)toggle
{
    if ([self isVisible]) [self hide];
    else [self show];
}

- (BOOL)isVisible
{
    return [_panel isVisible];
}

- (void)refresh
{
    [self refreshPads];
    [self refreshLabels];
}

/* The panel's mouse and keyboard holds: the main window releases its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
    studio2session_release_all(_session);
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end
