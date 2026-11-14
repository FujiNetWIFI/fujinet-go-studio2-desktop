/*
 * studio2vidtab -- the Video tab's controller. All four frontends draw the
 * same tab from it, so what they own is widgets: a view and zoom chooser,
 * overlay toggles, a picture, the inspector and four tables. The lessons of
 * the other debuggers' video tabs live here once:
 *
 *   - the machine is only COPIED: a finished frame (the 1861's raster and
 *     what its DMA delivered on every line) is captured once per frame on
 *     the machine thread, and a stopped machine is read while it is parked.
 *     Decoding, scaling and overlays run on the caller's (UI) thread;
 *   - the picture is rebuilt only when something it shows changed, into one
 *     reused buffer;
 *   - hover and pin are state here, so a mouse move never re-renders the
 *     picture: draw the marks from studio2vid_tab_marks() over the cached
 *     image;
 *   - pixels are 0xFFRRGGBB (opaque), whatever the toolkit does with alpha;
 *   - one table of overlays and one env-var parser, so the frontends cannot
 *     disagree.
 *
 * The CDP1861 has no registers a program writes: it shows what the CPU's DMA
 * fetches (R0 walks memory eight bytes a line while the display is on), one
 * bit a pixel, white on black. Everything per-line therefore comes from the
 * DMA: the address of a line's first byte, how many bytes landed (8, or a
 * short burst when an instruction was still running), the bytes, where on the
 * line they started, and which kind of page they came from (console RAM, the
 * cartridge's raster, cartridge ROM, the BIOS, open bus). The BIOS shows 32
 * rows of 8 bytes at $0900 by pointing R0 back at the same row four lines
 * running; a FujiNet app walks the cartridge's raster instead.
 *
 * Views: Display is the frame as the TV showed it (the 100 x 246 visible
 * area); Decoded redraws the 64 x 128 display window from each line's own
 * DMA bytes, tinted by the page they came from; Beam is the whole 112 x 262
 * raster with the blanking, INT and EFx bands, this frame above the beam and
 * the last one dimmed below it (stopped only); Values shows each byte as a
 * flat false colour of its value.
 *
 * UI thread only. Not thread-safe.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef STUDIO2VIDTAB_H
#define STUDIO2VIDTAB_H

#include <stdint.h>

#include "studio2session.h"
#include "studio2debug.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct studio2vid_tab studio2vid_tab;
struct studio2vid_capture;      /* studio2vidcap.h; tests feed one */

enum studio2vid_view {
    STUDIO2VID_V_DISPLAY = 0,   /* the last finished frame, as the TV showed it (100 x 246) */
    STUDIO2VID_V_DECODED,       /* the display window rebuilt from each line's DMA bytes (64 x 128), tinted by source */
    STUDIO2VID_V_BEAM,          /* the whole raster (112 x 262), this frame above the beam, the last dimmed below (stopped) */
    STUDIO2VID_V_VALUES,        /* each DMA byte as a flat false colour of its value (64 x 128) */
    STUDIO2VID_V_COUNT
};

/* Overlays: one table, bit per entry. */
enum {
    STUDIO2VID_OV_GRID   = 1u << 0,  /* DMA byte cells (8 pixels); single pixels when zoomed in */
    STUDIO2VID_OV_BEAM   = 1u << 1,  /* where the beam is (stopped) */
    STUDIO2VID_OV_BANDS  = 1u << 2,  /* blanking, INT (78-79) and EFx (76-79, 204-207) lines (Display, Beam) */
    STUDIO2VID_OV_SHORT  = 1u << 3,  /* display lines whose DMA burst was short or missing */
    STUDIO2VID_OV_SOURCE = 1u << 4,  /* tint every byte by the page it came from (Decoded always does) */
    STUDIO2VID_OV_ACTIVE = 1u << 5,  /* outline of the 64 x 128 display window (Display, Beam) */
    STUDIO2VID_OV_ALL    = 0x3Fu
};
typedef struct {
    unsigned bit;
    const char *name;           /* the env name: grid, beam, ... */
    const char *label;          /* the checkbox text */
} studio2vid_overlay_info;
const studio2vid_overlay_info *studio2vid_overlays(int *count);
const char *studio2vid_view_name(int view);            /* "Display", ... */
const char *studio2vid_view_key(int view);             /* "display", ... (env) */

/* Right-hand sections. */
enum { STUDIO2VID_S_PIXEL = 0, STUDIO2VID_S_REGISTERS, STUDIO2VID_S_COLOURS,
       STUDIO2VID_S_LINES, STUDIO2VID_S_TIMING, STUDIO2VID_S_COUNT };
const char *studio2vid_section_name(int section);
const char *studio2vid_section_key(int section);

/* d is the debugger the tab asks whether the machine is stopped and adds
 * breakpoints to; NULL (tests) makes every Break on... return -1 and the
 * machine count as running. The tab asks the host to capture frames until it
 * is freed. */
studio2vid_tab *studio2vid_tab_new(studio2debug *dbg);
void            studio2vid_tab_free(studio2vid_tab *tab);

/* ---- what to show (they mark the picture stale) ------------------------------------ */
void     studio2vid_tab_set_view(studio2vid_tab *tab, int view);
int      studio2vid_tab_view(const studio2vid_tab *tab);
void     studio2vid_tab_set_overlays(studio2vid_tab *tab, unsigned ovl);
unsigned studio2vid_tab_overlays(const studio2vid_tab *tab);
/* 1, 2, 3, 4, 6, 8, 12, 16, or 0 for "fit the viewport" (the largest of
 * those that fits both ways: the picture is tall, so the height decides) */
void     studio2vid_tab_set_zoom(studio2vid_tab *tab, int zoom);
int      studio2vid_tab_zoom(const studio2vid_tab *tab);
/* the room the picture has, for Fit; call when the widget resizes */
void     studio2vid_tab_set_viewport(studio2vid_tab *tab, int wide, int high);
/* the zoom steps the chooser offers (0 = Fit first) */
const int *studio2vid_zooms(int *count);

/*
 * The scripted-capture variables, read once by studio2vid_tab_new:
 *   STUDIO2_DEBUGGER_VIDEO_VIEW      display decoded beam values
 *   STUDIO2_DEBUGGER_VIDEO_OVERLAYS  grid,beam,bands,short,source,active | all | none
 *   STUDIO2_DEBUGGER_VIDEO_ZOOM      1..16 or fit
 *   STUDIO2_DEBUGGER_VIDEO_SECTION   pixel registers colours lines timing   (names, never indexes)
 * Returns the starting section, STUDIO2VID_S_PIXEL when unset.
 */
int studio2vid_tab_env_section(const studio2vid_tab *tab);

/* ---- refresh ------------------------------------------------------------------------ */
#define STUDIO2VID_TAB_PICTURE 1u  /* repaint the picture (studio2vid_tab_picture) */
#define STUDIO2VID_TAB_TABLES  2u  /* refill the status line and the sections */
#define STUDIO2VID_TAB_OFF     4u  /* no machine: show studio2vid_tab_why() instead of a picture */

/* Take the machine's state, update the trackers, and say what to redraw.
 * force: redraw everything. */
unsigned studio2vid_tab_refresh(studio2vid_tab *tab, int force);

/* For tests: use this capture instead of the machine. stopped: the machine
 * counts as stopped, so the Beam view works and cap->beam_row says how far
 * the beam has drawn (a running machine's capture is a finished frame). */
void studio2vid_tab_feed(studio2vid_tab *tab, const struct studio2vid_capture *cap, int stopped);

/* ---- the picture ------------------------------------------------------------------- */
/*
 * The rendered picture, 0xFFRRGGBB, valid until the next refresh or set_*. NULL
 * when this view cannot be drawn: studio2vid_tab_why() says why, and the tab
 * should show that text.
 */
const uint32_t *studio2vid_tab_picture(studio2vid_tab *tab, int *wide, int *high);
const char     *studio2vid_tab_why(const studio2vid_tab *tab);
int             studio2vid_tab_effective_zoom(const studio2vid_tab *tab);

/* ---- the tables -------------------------------------------------------------------- */
const char *studio2vid_tab_status(const studio2vid_tab *tab);        /* two lines, '\n' between */

typedef struct {
    const char *name;           /* "DISP", ..., "R0" */
    char value[24];             /* "on" */
    char meaning[112];
    int age;                    /* refreshes since it last changed (STUDIO2VID_REG_FADE+: not recent) */
} studio2vid_reg;
#define STUDIO2VID_REG_FADE 12
int studio2vid_tab_regs(const studio2vid_tab *tab, const studio2vid_reg **out);

/* The two colours (index 0 black, 1 white). */
typedef struct {
    int index;                  /* 0 or 1: the bit */
    const char *name;           /* "Black (0)", "White (1)" */
    uint32_t rgb;               /* 0xFFRRGGBB */
    long uses;                  /* bits of the display window's DMA bytes with this value */
    int inspected;              /* the inspected pixel has it */
} studio2vid_swatch;
int studio2vid_tab_swatches(const studio2vid_tab *tab, const studio2vid_swatch **out);

/* The per-line table: one entry for every line that has a DMA record (the
 * frame's or the last one's) and for every display line (80-207), whether
 * it has one or not. */
enum { STUDIO2VID_LC_ADDR = 1,   /* R0 did not carry on from the line above (a reload, or the first) */
       STUDIO2VID_LC_COUNT = 2,  /* a different byte count */
       STUDIO2VID_LC_HPOS = 4,   /* the burst started elsewhere on the line */
       STUDIO2VID_LC_SRC = 8,    /* a different kind of page */
       STUDIO2VID_LC_FLAGS = 16  /* DISP / INT / EFx differ */ };
typedef struct {
    int row;                    /* raster line 0..261 */
    int display;                /* inside the display lines 80-207 */
    int valid;                  /* a DMA record exists */
    int fresh;                  /* from this frame (else the previous one's) */
    int count;                  /* DMA bytes on the line */
    int src;                    /* STUDIO2VID_SRC_* */
    const char *src_name;       /* "RAM", "cart raster", ... */
    int hpos;                   /* where the burst started */
    unsigned addr;              /* R0 at the first byte */
    uint8_t data[8];
    int flags;                  /* STUDIO2VID_LF_* */
    int changed_mask;           /* STUDIO2VID_LC_* against the previous row listed */
} studio2vid_line_row;
int studio2vid_tab_lines(const studio2vid_tab *tab, const studio2vid_line_row **out);

/* the timing section, as ready-made "label\tvalue" lines */
int  studio2vid_tab_timing(const studio2vid_tab *tab, const char *const **lines);
/* where the beam is in the whole frame, 0..1000 (0 when unknown) */
int  studio2vid_tab_beam_permille(const studio2vid_tab *tab);

/* A section as plain text (the console's `video [section]` shares it). */
int  studio2vid_tab_text(studio2vid_tab *tab, int section, char *dst, int dstsz);
/* A section by name or prefix ("pixel", "reg", ...); -1 if none. */
int  studio2vid_section_from_name(const char *name);

/* ---- hover, pin ---------------------------------------------------------------------- */
typedef struct {
    int valid;
    int picx, picy;                   /* picture pixel */
    int line;                   /* raster line, -1 if the picture has none there */
    int column;                 /* clock on the line (raster x), -1 if unknown */
    int dma;                    /* a DMA byte drew this pixel */
    int index;                  /* which byte of the line's burst, 0..7 */
    int bit;                    /* 7 (left) .. 0 */
    unsigned addr;              /* where the byte came from */
    uint8_t value;              /* the byte */
    int src;                    /* STUDIO2VID_SRC_* */
    const char *src_name;
    int on;                     /* the pixel is white */
    int fresh;                  /* from this frame, not the last one's */
    uint32_t rgb;               /* 0xFFRRGGBB */
    char text[512];             /* ready-made inspector text */
} studio2vid_pixel;

/* x, y in picture pixels at the current zoom; -1,-1 when the pointer left.
 * Returns 1 when the inspector changed. */
int  studio2vid_tab_hover(studio2vid_tab *tab, int posx, int posy);
void studio2vid_tab_pin(studio2vid_tab *tab, int posx, int posy);      /* a click; the same spot unpins */
void studio2vid_tab_unpin(studio2vid_tab *tab);
/* the pinned pixel if any, else the hovered one, else NULL */
const studio2vid_pixel *studio2vid_tab_pixel(const studio2vid_tab *tab);
/* rectangles x,y,w,h in picture pixels for the marks drawn over the picture;
 * bit0 of the result: hover valid, bit1: pin valid */
int  studio2vid_tab_marks(const studio2vid_tab *tab, int hover[4], int pin[4]);
/* A write breakpoint on the address the inspected pixel's byte came from
 * ("Break on writes"): only for console RAM (the BIOS display page $0900); -1
 * for the cartridge's raster, ROM or nothing, and without a debugger. Not
 * added twice. The breakpoint's id, or -1. */
int  studio2vid_tab_break_on_pixel(studio2vid_tab *tab);
/* 1 when "Break on writes" has a RAM address to break on now */
int  studio2vid_tab_can_break(const studio2vid_tab *tab);

#ifdef __cplusplus
}
#endif

#endif
