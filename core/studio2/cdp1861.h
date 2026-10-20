/* cdp1861.h -- the RCA CDP1861 Pixie video display controller.
 *
 * Transposed from MAME's src/devices/video/cdp1861.cpp and cdp1861.h
 * (BSD-3-Clause, copyright-holders Curt Coder). MAME drives the chip with
 * three emu_timers placed by screen().time_until_pos(); here they are three
 * absolute clock times run against the CPU's clock count, with MAME's
 * screen arithmetic transposed exactly: 112 clocks a line, 262 lines, the
 * beam's time origin (line 0, the start of VBLANK) at power-on, and an
 * event placed at the position the beam is already on going to the next
 * frame.
 *
 * The DMA timer runs free from the last reset (2 machine cycles, then 8 on
 * and 6 off, one line per period), so its phase within a line is wherever
 * the reset left it; the bitmap keeps MAME's flat 112-pixel rows, so a
 * burst that starts late on a line spills into the next one as MAME's does.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME; copyright-holders Curt Coder -- see COMPLIANCE.md)
 */

#ifndef STUDIO2_CDP1861_H
#define STUDIO2_CDP1861_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* cdp1861.h's constants */
#define CDP1861_VISIBLE_COLUMNS        64
#define CDP1861_VISIBLE_LINES          128
#define CDP1861_HBLANK_START           (14 * 8)
#define CDP1861_HBLANK_END             12
#define CDP1861_SCREEN_WIDTH           (14 * 8)     /* 112 clocks a line */
#define CDP1861_TOTAL_SCANLINES        262
#define CDP1861_SCANLINE_DISPLAY_START 80
#define CDP1861_SCANLINE_DISPLAY_END   208
#define CDP1861_SCANLINE_VBLANK_START  262
#define CDP1861_SCANLINE_VBLANK_END    16
#define CDP1861_SCANLINE_INT_START     (CDP1861_SCANLINE_DISPLAY_START - 2)
#define CDP1861_SCANLINE_INT_END       CDP1861_SCANLINE_DISPLAY_START
#define CDP1861_SCANLINE_EFX_TOP_START (CDP1861_SCANLINE_DISPLAY_START - 4)
#define CDP1861_SCANLINE_EFX_TOP_END   CDP1861_SCANLINE_DISPLAY_START
#define CDP1861_SCANLINE_EFX_BOTTOM_START (CDP1861_SCANLINE_DISPLAY_END - 4)
#define CDP1861_SCANLINE_EFX_BOTTOM_END   CDP1861_SCANLINE_DISPLAY_END

#define CDP1861_CYCLES_DMA_START       (2 * 8)
#define CDP1861_CYCLES_DMA_ACTIVE      (8 * 8)
#define CDP1861_CYCLES_DMA_WAIT        (6 * 8)

#define CDP1861_FRAME_CLOCKS (CDP1861_SCREEN_WIDTH * CDP1861_TOTAL_SCANLINES) /* 29344 */

/* MAME's screen: set_raw(clock, 112, 12, 112, 262, 16, 262), so the visible
 * area -- and every MAME snapshot -- is 100 x 246. */
#define CDP1861_VIS_X0  CDP1861_HBLANK_END
#define CDP1861_VIS_Y0  CDP1861_SCANLINE_VBLANK_END
#define CDP1861_VIS_W   (CDP1861_HBLANK_START - CDP1861_HBLANK_END)                  /* 100 */
#define CDP1861_VIS_H   (CDP1861_SCANLINE_VBLANK_START - CDP1861_SCANLINE_VBLANK_END) /* 246 */

typedef struct {
    /* MAME state */
    int disp;
    int dispon, dispoff;
    int dmaout;             /* the DMA timer's own phase */

    /* the three timers, as absolute clocks */
    uint64_t int_next, efx_next, dma_next;

    /* the output lines, 1 asserted */
    int int_line, dma_line, efx_line;

    /* MAME's m_bitmap: whole raster, 112-pixel rows (XRGB8888) */
    uint32_t bitmap[CDP1861_SCREEN_WIDTH * CDP1861_TOTAL_SCANLINES];

    /* for the debugger */
    uint64_t dma_bytes;     /* bytes plotted since power-on */
    uint16_t last_dma_addr;
} cdp1861_t;

/* device_start: a black bitmap, the timers unset. */
void cdp1861_init(cdp1861_t *v);
/* device_reset at clock `now`. */
void cdp1861_reset(cdp1861_t *v, uint64_t now);
/* Fire every timer due at or before `t`. */
void cdp1861_run_until(cdp1861_t *v, uint64_t t);
/* The next timer's time (the machine runs the CPU up to it). */
uint64_t cdp1861_next_event(const cdp1861_t *v);
/* dma_w: one DMA byte at clock `now`. */
void cdp1861_dma_w(cdp1861_t *v, uint64_t now, uint16_t addr, uint8_t data);
void cdp1861_disp_on_w(cdp1861_t *v, int state);

/* The beam at a clock (screen().vpos() / hpos()). */
static inline int cdp1861_vpos(uint64_t t)
{
    return (int)((t / CDP1861_SCREEN_WIDTH) % CDP1861_TOTAL_SCANLINES);
}

static inline int cdp1861_hpos(uint64_t t)
{
    return (int)(t % CDP1861_SCREEN_WIDTH);
}

/* screen_update: the visible area (CDP1861_VIS_W x CDP1861_VIS_H) into dst,
 * black while the display is off. */
void cdp1861_snapshot(const cdp1861_t *v, uint32_t *dst);

#ifdef __cplusplus
}
#endif

#endif /* STUDIO2_CDP1861_H */
