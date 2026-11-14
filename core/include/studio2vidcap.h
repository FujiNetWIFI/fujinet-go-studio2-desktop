/*
 * studio2vidcap -- the CDP1861's raster as the machine left it, for the Video
 * tab (studio2vidtab.h). Taken once per frame on the machine thread while a
 * tab exists (studio2_host_capture_*), so a reader sees one consistent frame
 * while the game runs. A stopped machine is filled live: lines the beam has
 * passed carry this frame's DMA, the rest the last frame's.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef STUDIO2VIDCAP_H
#define STUDIO2VIDCAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The 1861's geometry (cdp1861.h), repeated so the tab and the frontends
 * need not include the machine's headers. */
#define STUDIO2VID_RASTER_W   112       /* clocks (pixels) a line */
#define STUDIO2VID_RASTER_H   262       /* lines a frame */
#define STUDIO2VID_VIS_X0     12        /* the visible area inside the raster */
#define STUDIO2VID_VIS_Y0     16
#define STUDIO2VID_VIS_W      100
#define STUDIO2VID_VIS_H      246
#define STUDIO2VID_DISP_Y0    80        /* the display lines, 80 .. 207 */
#define STUDIO2VID_DISP_Y1    208
#define STUDIO2VID_INT_Y0     78        /* INT asserted on lines 78-79 */
#define STUDIO2VID_EFX_TOP_Y0 76        /* EFx on 76-79 and 204-207 */
#define STUDIO2VID_EFX_BOT_Y0 204

/* Where a line's first DMA byte came from. */
enum {
    STUDIO2VID_SRC_NONE = 0,    /* nothing */
    STUDIO2VID_SRC_RAM,         /* console RAM ($0800-$09FF and its mirrors) */
    STUDIO2VID_SRC_RASTER,      /* the cartridge's raster (a FujiNet app's text engine) */
    STUDIO2VID_SRC_CART,        /* a cartridge ROM / hotspot page */
    STUDIO2VID_SRC_BIOS,        /* the system ROM */
    STUDIO2VID_SRC_OPEN,        /* open bus */
    STUDIO2VID_SRC_COUNT
};

enum {
    STUDIO2VID_LF_DISP = 0x01,  /* display on at the first byte */
    STUDIO2VID_LF_INT  = 0x02,  /* INT line asserted */
    STUDIO2VID_LF_EFX  = 0x04   /* EFx line asserted */
};

typedef struct {
    uint8_t valid;              /* the line has a record from this frame or the one before */
    uint8_t fresh;              /* ... from this frame (the capture's own) */
    uint8_t count;              /* DMA bytes on the line: 8 full, 0 none, else short */
    uint8_t src;                /* STUDIO2VID_SRC_* of the first byte */
    uint8_t hpos;               /* beam hpos of the first byte */
    uint8_t flags;              /* STUDIO2VID_LF_* */
    uint16_t addr;              /* R0 at the first byte; byte n came from addr + n */
    uint8_t data[8];
} studio2vid_line;

typedef struct studio2vid_capture {
    /* the whole raster, one byte a pixel (0 black, 1 white), as MAME's bitmap
     * holds it; what the TV shows is the visible area of it while `disp` */
    uint8_t bits[STUDIO2VID_RASTER_W * STUDIO2VID_RASTER_H];
    studio2vid_line lines[STUDIO2VID_RASTER_H];
    int disp;                   /* the display is on */
    int blank;                  /* the stand-in parked: the picture is black */
    int int_line, efx_line, dma_line;
    int dmaout;                 /* the DMA timer's own phase */
    int vpos, hpos;             /* the beam */
    int beam_row;               /* raster rows drawn this frame so far (262: a finished frame) */
    uint16_t r0;                /* the DMA pointer now */
    uint16_t last_dma_addr;
    uint64_t cycles;            /* clocks since power-on */
    uint64_t frame;             /* frames the machine has run */
    uint64_t dma_bytes;
    uint32_t inp1_count;
    uint64_t int_next, efx_next, dma_next;   /* the 1861's timers, absolute clocks */
    uint32_t short_lines;       /* display lines of this capture with a short or no burst */
    uint8_t cpu_p, cpu_ie;      /* P and IE now (the interrupt routine runs with P = 1) */
    uint64_t serial;            /* 0: nothing captured yet */
} studio2vid_capture;

#ifdef __cplusplus
}
#endif

#endif
