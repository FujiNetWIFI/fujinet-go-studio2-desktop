/*
 * vidtab.c -- the Video tab's controller (core/include/studio2vidtab.h).
 *
 * Everything here works from a studio2vid_capture: the 1861's raster and,
 * per line, the DMA burst that drew it. The Decoded view redraws the display
 * window from those bytes alone, so on an unchanged frame it is the same
 * picture the machine showed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "studio2vidtab.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "host.h"
#include "studio2vidcap.h"

#define RASTW STUDIO2VID_RASTER_W
#define RASTH STUDIO2VID_RASTER_H
#define WIN_W 64                /* the display window: 8 bytes ... */
#define WIN_H 128               /* ... on each of the 128 display lines */
#define MAXPIX 6000000L         /* the largest picture we will build */
#define MAX_REGS 16
#define MAX_TIMING 12
#define BYTE_DX 4               /* a burst's first pixel is 4 clocks after its first byte's hpos */

static const int k_zooms[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };

enum { BK_DISPLAY = 0, BK_RASTER, BK_WINDOW };   /* what the base picture is */

#define COL_WHITE   0xffffffffu
#define COL_BLACK   0xff000000u
#define COL_MISSING 0xff3a0a0au     /* a window byte the DMA never delivered */

struct studio2vid_tab {
    studio2debug *dbg;
    int view, zoom, vieww, viewh;
    unsigned ovl;
    int env_section, capture_on;

    /* the machine's state as presented */
    studio2vid_capture *cap;    /* working capture */
    studio2vid_capture *last;   /* last captured by the machine thread */
    uint64_t last_serial;
    int have, has_fb, has_live, stopped, fed;
    uint64_t sig;

    /* the picture */
    uint32_t *base, *pic;
    size_t base_cap, pic_cap;
    int basew, baseh, picw, pich, effzoom, kind;
    int dirty;
    char why[200];

    /* tables */
    studio2vid_reg regs[MAX_REGS];
    char prev_val[MAX_REGS][24];
    int nregs, regs_primed;
    studio2vid_swatch swat[2];
    long uses[2];
    studio2vid_line_row rows[RASTH];
    int nrows;
    char status[400];
    char timing_buf[MAX_TIMING][112];
    const char *timing_ptr[MAX_TIMING];
    int ntiming;

    /* pixels */
    int hovx, hovy, pinx, piny;         /* -1: none */
    studio2vid_pixel hover, pin;
};

/* ---- tables of names ----------------------------------------------------------------- */

static const studio2vid_overlay_info k_overlays[] = {
    { STUDIO2VID_OV_GRID,   "grid",   "Grid" },
    { STUDIO2VID_OV_BEAM,   "beam",   "Beam" },
    { STUDIO2VID_OV_BANDS,  "bands",  "Blanking, INT and EFx lines" },
    { STUDIO2VID_OV_SHORT,  "short",  "Short DMA lines" },
    { STUDIO2VID_OV_SOURCE, "source", "Source page tint" },
    { STUDIO2VID_OV_ACTIVE, "active", "Display window outline" },
};
#define N_OVERLAYS ((int)(sizeof k_overlays / sizeof *k_overlays))

const studio2vid_overlay_info *studio2vid_overlays(int *count)
{
    if (count) *count = N_OVERLAYS;
    return k_overlays;
}

static const char *const k_view_names[] = { "Display", "Decoded", "Beam", "Values" };
static const char *const k_view_keys[] = { "display", "decoded", "beam", "values" };
static const char *const k_sec_names[] = { "Pixel", "Registers", "Colours", "Lines", "Timing" };
static const char *const k_sec_keys[] = { "pixel", "registers", "colours", "lines", "timing" };
static const char *const k_src_names[STUDIO2VID_SRC_COUNT] = {
    "none", "RAM", "cart raster", "cart ROM", "BIOS ROM", "open bus" };

const char *studio2vid_view_name(int val) { return val >= 0 && val < STUDIO2VID_V_COUNT ? k_view_names[val] : ""; }
const char *studio2vid_view_key(int val) { return val >= 0 && val < STUDIO2VID_V_COUNT ? k_view_keys[val] : ""; }
const char *studio2vid_section_name(int str) { return str >= 0 && str < STUDIO2VID_S_COUNT ? k_sec_names[str] : ""; }
const char *studio2vid_section_key(int str) { return str >= 0 && str < STUDIO2VID_S_COUNT ? k_sec_keys[str] : ""; }

int studio2vid_section_from_name(const char *name)
{
    if (!name || !*name) return -1;
    for (int sec = 0; sec < STUDIO2VID_S_COUNT; sec++)
        if (!strncasecmp(name, k_sec_keys[sec], strlen(name))) return sec;
    return -1;
}

static const char *src_name(int src)
{
    return src >= 0 && src < STUDIO2VID_SRC_COUNT ? k_src_names[src] : "?";
}

const int *studio2vid_zooms(int *count)
{
    if (count) *count = (int)(sizeof k_zooms / sizeof *k_zooms);
    return k_zooms;
}

/* ---- small helpers ---------------------------------------------------------------------- */

static uint64_t fnv(uint64_t high, const void *pixp, size_t num)
{
    const uint8_t *blu = pixp;
    while (num--) { high ^= *blu++; high *= 1099511628211ull; }
    return high;
}

static uint32_t blend(uint32_t dst, uint32_t rgb, int a256)
{
    const uint32_t dstr = (dst >> 16) & 255, dstg = (dst >> 8) & 255, dstb = dst & 255;
    const uint32_t srcr = (rgb >> 16) & 255, srcg = (rgb >> 8) & 255, srcb = rgb & 255;
    const uint32_t ret = (dstr * (uint32_t)(256 - a256) + srcr * (uint32_t)a256) >> 8;
    const uint32_t grn = (dstg * (uint32_t)(256 - a256) + srcg * (uint32_t)a256) >> 8;
    const uint32_t blu = (dstb * (uint32_t)(256 - a256) + srcb * (uint32_t)a256) >> 8;
    return 0xff000000u | ret << 16 | grn << 8 | blu;
}

static int fits_zoom(int wide, int high, int scale)
{
    return (long)wide * scale * (long)high * scale <= MAXPIX;
}

static void why_set(studio2vid_tab *tab, const char *str)
{
    snprintf(tab->why, sizeof tab->why, "%s", str);
}

static int ensure(uint32_t **buf, size_t *cap, size_t need)
{
    if (need > *cap) {
        uint32_t *grown = realloc(*buf, need * sizeof **buf);
        if (!grown) return 0;
        *buf = grown;
        *cap = need;
    }
    return 1;
}

/* the bytes a line really holds (a record could say more than it kept) */
static int line_bytes(const studio2vid_line *rec)
{
    return !rec->valid ? 0 : rec->count > 8 ? 8 : rec->count;
}

static int in_display(int row) { return row >= STUDIO2VID_DISP_Y0 && row < STUDIO2VID_DISP_Y1; }

/* the colours of a byte's bits, by the kind of page it came from */
static void src_tint(int src, uint32_t *on, uint32_t *off)
{
    switch (src) {
    case STUDIO2VID_SRC_RAM:    *on = 0xffd8f0d8u; *off = 0xff0c1a0cu; break;
    case STUDIO2VID_SRC_RASTER: *on = 0xffd0e4ffu; *off = 0xff0a1424u; break;
    case STUDIO2VID_SRC_CART:   *on = 0xffffe4b0u; *off = 0xff241a08u; break;
    case STUDIO2VID_SRC_BIOS:   *on = 0xfff0d8f0u; *off = 0xff1a0c1au; break;
    default:                    *on = 0xffffb0b0u; *off = 0xff240808u; break;
    }
}

/* the false colour of a byte's value: cool low, warm high */
static uint32_t value_colour(int value)
{
    const int val8 = value & 255;
    const int tri = val8 < 128 ? val8 * 2 : (255 - val8) * 2;
    return 0xff000000u | (uint32_t)(40 + val8 * 215 / 255) << 16 |
           (uint32_t)(40 + tri * 160 / 255) << 8 | (uint32_t)(255 - val8 * 200 / 255);
}

/* ---- new / free / settings -------------------------------------------------------------- */

studio2vid_tab *studio2vid_tab_new(studio2debug *dbg)
{
    studio2vid_tab *tab = calloc(1, sizeof *tab);
    const char *envv;
    if (!tab) return NULL;
    tab->dbg = dbg;
    tab->cap = calloc(1, sizeof *tab->cap);
    tab->last = calloc(1, sizeof *tab->last);
    if (!tab->cap || !tab->last) {
        studio2vid_tab_free(tab);
        return NULL;
    }
    tab->view = STUDIO2VID_V_DISPLAY;
    tab->ovl = STUDIO2VID_OV_BEAM | STUDIO2VID_OV_ACTIVE;
    tab->zoom = 0;
    tab->vieww = STUDIO2VID_VIS_W * 2;
    tab->viewh = STUDIO2VID_VIS_H * 2;
    tab->hovx = tab->hovy = tab->pinx = tab->piny = -1;
    tab->dirty = 1;

    if ((envv = getenv("STUDIO2_DEBUGGER_VIDEO_VIEW")) && *envv) {
        for (int i = 0; i < STUDIO2VID_V_COUNT; i++)
            if (!strncasecmp(envv, k_view_keys[i], strlen(envv))) { tab->view = i; break; }
    }
    if ((envv = getenv("STUDIO2_DEBUGGER_VIDEO_OVERLAYS"))) {
        char buf[200], *tok, *save;
        unsigned ovl = 0;
        snprintf(buf, sizeof buf, "%s", envv);
        for (tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
            if (!strcasecmp(tok, "all")) ovl |= STUDIO2VID_OV_ALL;
            else if (!strcasecmp(tok, "none")) ovl = 0;
            else
                for (int i = 0; i < N_OVERLAYS; i++)
                    if (!strcasecmp(tok, k_overlays[i].name)) ovl |= k_overlays[i].bit;
        }
        tab->ovl = ovl;
    }
    if ((envv = getenv("STUDIO2_DEBUGGER_VIDEO_ZOOM")) && *envv) {
        if (!strcasecmp(envv, "fit")) tab->zoom = 0;
        else {
            const int scale = atoi(envv);
            for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
                if (k_zooms[i] == scale) tab->zoom = scale;
        }
    }
    tab->env_section = STUDIO2VID_S_PIXEL;
    if ((envv = getenv("STUDIO2_DEBUGGER_VIDEO_SECTION")) && *envv) {
        const int sec = studio2vid_section_from_name(envv);
        if (sec >= 0) tab->env_section = sec;
    }
    studio2_host_capture_enable(1);
    tab->capture_on = 1;
    return tab;
}

void studio2vid_tab_free(studio2vid_tab *tab)
{
    if (!tab) return;
    if (tab->capture_on)
        studio2_host_capture_enable(0);
    free(tab->cap);
    free(tab->last);
    free(tab->base);
    free(tab->pic);
    free(tab);
}

void studio2vid_tab_set_view(studio2vid_tab *tab, int val)
{
    if (tab && val >= 0 && val < STUDIO2VID_V_COUNT && val != tab->view) { tab->view = val; tab->dirty = 1; }
}
int studio2vid_tab_view(const studio2vid_tab *tab) { return tab ? tab->view : 0; }
void studio2vid_tab_set_overlays(studio2vid_tab *tab, unsigned ovl)
{
    if (tab && (ovl & STUDIO2VID_OV_ALL) != tab->ovl) { tab->ovl = ovl & STUDIO2VID_OV_ALL; tab->dirty = 1; }
}
unsigned studio2vid_tab_overlays(const studio2vid_tab *tab) { return tab ? tab->ovl : 0; }
void studio2vid_tab_set_zoom(studio2vid_tab *tab, int scale)
{
    if (tab && scale >= 0 && scale <= 16 && scale != tab->zoom) { tab->zoom = scale; tab->dirty = 1; }
}
int studio2vid_tab_zoom(const studio2vid_tab *tab) { return tab ? tab->zoom : 0; }
void studio2vid_tab_set_viewport(studio2vid_tab *tab, int wide, int high)
{
    if (!tab) return;
    if (wide < 1) wide = 1;
    if (high < 1) high = 1;
    if (wide != tab->vieww || high != tab->viewh) {
        tab->vieww = wide; tab->viewh = high;
        if (tab->zoom == 0) tab->dirty = 1;
    }
}
int studio2vid_tab_env_section(const studio2vid_tab *tab) { return tab ? tab->env_section : 0; }

/* ---- feeding ----------------------------------------------------------------------------- */

void studio2vid_tab_feed(studio2vid_tab *tab, const struct studio2vid_capture *cap, int stopped)
{
    if (!tab || !cap) return;
    memcpy(tab->cap, cap, sizeof *tab->cap);
    tab->fed = 1;
    tab->have = 1;
    tab->has_fb = cap->serial != 0;
    tab->stopped = stopped != 0;
    tab->has_live = tab->stopped;
}

/* Take what the machine has. 0 if there is no machine. */
static int acquire(studio2vid_tab *tab)
{
    studio2_machine_t *mach;

    if (tab->fed)
        return tab->have;
    tab->have = 0;
    if (!studio2_host_is_running())
        return 0;
    studio2_host_capture_get(tab->last, &tab->last_serial);
    tab->has_fb = tab->last->serial != 0;
    tab->stopped = tab->dbg && studio2debug_is_stopped(tab->dbg);
    tab->has_live = tab->stopped;

    if (tab->stopped || !tab->has_fb) {
        /* a parked machine is ours to read: the lines the beam has passed
         * carry this frame's DMA, the rest the last frame's. Before the
         * first capture the frame in progress is all there is. */
        studio2_host_lock();
        mach = studio2_host_machine();
        if (!mach) {
            studio2_host_unlock();
            return 0;
        }
        studio2_host_capture_fill(mach, tab->cap);
        studio2_host_unlock();
        tab->cap->serial = tab->last->serial;
        if (!tab->stopped)
            tab->cap->beam_row = RASTH;
    } else {
        memcpy(tab->cap, tab->last, sizeof *tab->cap);
    }
    tab->have = 1;
    return 1;
}

static uint64_t signature(const studio2vid_tab *tab)
{
    const studio2vid_capture *cap = tab->cap;
    uint64_t high = 1469598103934665603ull;
    high = fnv(high, cap->bits, sizeof cap->bits);
    high = fnv(high, cap->lines, sizeof cap->lines);
    high = fnv(high, &cap->serial, sizeof cap->serial);
    high = fnv(high, &cap->vpos, sizeof cap->vpos);
    high = fnv(high, &cap->hpos, sizeof cap->hpos);
    high = fnv(high, &cap->frame, sizeof cap->frame);
    high = fnv(high, &cap->beam_row, sizeof cap->beam_row);
    high = fnv(high, &cap->disp, sizeof cap->disp);
    high = fnv(high, &cap->blank, sizeof cap->blank);
    high = fnv(high, &cap->r0, sizeof cap->r0);
    high = fnv(high, &cap->dma_bytes, sizeof cap->dma_bytes);
    high = fnv(high, &tab->has_live, sizeof tab->has_live);
    high = fnv(high, &tab->has_fb, sizeof tab->has_fb);
    return high;
}

/* ---- the base picture (1x) -------------------------------------------------------------------- */

static uint32_t raster_px(const studio2vid_capture *cap, int rastx, int rasty)
{
    return cap->bits[rasty * RASTW + rastx] ? COL_WHITE : COL_BLACK;
}

/* the display window from the DMA bytes: tinted by source, or false colours */
static void decode_window(const studio2vid_capture *cap, uint32_t *dst, int values)
{
    for (int line = 0; line < WIN_H; line++) {
        const studio2vid_line *rec = &cap->lines[STUDIO2VID_DISP_Y0 + line];
        const int have = line_bytes(rec);
        for (int idx = 0; idx < 8; idx++) {
            uint32_t on = COL_WHITE, off = COL_BLACK;
            if (idx >= have) {
                for (int bit = 0; bit < 8; bit++)
                    dst[(size_t)line * WIN_W + (size_t)(idx * 8 + bit)] = COL_MISSING;
                continue;
            }
            if (!values)
                src_tint(rec->src, &on, &off);
            for (int bit = 0; bit < 8; bit++)
                dst[(size_t)line * WIN_W + (size_t)(idx * 8 + bit)] =
                    values ? value_colour(rec->data[idx]) : ((rec->data[idx] >> (7 - bit)) & 1) ? on : off;
        }
    }
}

static int build_base(studio2vid_tab *tab)
{
    const studio2vid_capture *cap = tab->cap;

    if (!tab->has_fb && !tab->has_live) {
        why_set(tab, "No frame has finished yet. Let the machine run for a moment.");
        return 0;
    }
    switch (tab->view) {
    case STUDIO2VID_V_DISPLAY: {
        const int show = cap->disp && !cap->blank;
        if (!ensure(&tab->base, &tab->base_cap, (size_t)STUDIO2VID_VIS_W * STUDIO2VID_VIS_H)) return 0;
        for (int posy = 0; posy < STUDIO2VID_VIS_H; posy++)
            for (int posx = 0; posx < STUDIO2VID_VIS_W; posx++)
                tab->base[(size_t)posy * STUDIO2VID_VIS_W + (size_t)posx] =
                    show ? raster_px(cap, posx + STUDIO2VID_VIS_X0, posy + STUDIO2VID_VIS_Y0) : COL_BLACK;
        tab->basew = STUDIO2VID_VIS_W; tab->baseh = STUDIO2VID_VIS_H;
        tab->kind = BK_DISPLAY;
        return 1;
    }
    case STUDIO2VID_V_BEAM: {
        if (!tab->has_live) {
            why_set(tab, "The Beam view needs the machine stopped: it shows this frame as far as the beam has drawn it, and the last one dimmed below.");
            return 0;
        }
        if (!ensure(&tab->base, &tab->base_cap, (size_t)RASTW * RASTH)) return 0;
        for (int posy = 0; posy < RASTH; posy++)
            for (int posx = 0; posx < RASTW; posx++) {
                const uint32_t picx = raster_px(cap, posx, posy);
                tab->base[(size_t)posy * RASTW + (size_t)posx] = posy < cap->beam_row ? picx : blend(picx, 0x000000, 150);
            }
        tab->basew = RASTW; tab->baseh = RASTH;
        tab->kind = BK_RASTER;
        return 1;
    }
    default:
        if (!ensure(&tab->base, &tab->base_cap, (size_t)WIN_W * WIN_H)) return 0;
        decode_window(cap, tab->base, tab->view == STUDIO2VID_V_VALUES);
        tab->basew = WIN_W; tab->baseh = WIN_H;
        tab->kind = BK_WINDOW;
        return 1;
    }
}

/* ---- overlays ---------------------------------------------------------------------------- */

typedef struct { uint32_t *pixp; int wide, high; } canvas;

static void px_blend(canvas *cnv, int posx, int posy, uint32_t rgb, int alpha)
{
    if (posx >= 0 && posy >= 0 && posx < cnv->wide && posy < cnv->high)
        cnv->pixp[(size_t)posy * (size_t)cnv->wide + (size_t)posx] = blend(cnv->pixp[(size_t)posy * (size_t)cnv->wide + (size_t)posx], rgb, alpha);
}
static void hline(canvas *cnv, int posy, int minx, int maxx, uint32_t rgb, int alpha)
{
    for (int posx = minx; posx < maxx; posx++) px_blend(cnv, posx, posy, rgb, alpha);
}
static void vline(canvas *cnv, int posx, int starty, int endy, uint32_t rgb, int alpha)
{
    for (int posy = starty; posy < endy; posy++) px_blend(cnv, posx, posy, rgb, alpha);
}
static void fill_rect(canvas *cnv, int posx, int posy, int wide, int high, uint32_t rgb, int alpha)
{
    for (int offy = 0; offy < high; offy++)
        for (int offx = 0; offx < wide; offx++) px_blend(cnv, posx + offx, posy + offy, rgb, alpha);
}

/* raster coordinates <-> picture pixels of the raster-like pictures */
static int org_x(const studio2vid_tab *tab) { return tab->kind == BK_DISPLAY ? STUDIO2VID_VIS_X0 : 0; }
static int org_y(const studio2vid_tab *tab) { return tab->kind == BK_DISPLAY ? STUDIO2VID_VIS_Y0 : 0; }

static int band_int(int row) { return row >= STUDIO2VID_INT_Y0 && row < STUDIO2VID_DISP_Y0; }
static int band_efx(int row)
{
    return (row >= STUDIO2VID_EFX_TOP_Y0 && row < STUDIO2VID_DISP_Y0) ||
           (row >= STUDIO2VID_EFX_BOT_Y0 && row < STUDIO2VID_DISP_Y1);
}

static void draw_overlays(studio2vid_tab *tab, canvas *cnv, int scale)
{
    const studio2vid_capture *cap = tab->cap;
    const int window = tab->kind == BK_WINDOW;
    const int orgx = org_x(tab), orgy = org_y(tab);

    for (int posy = 0; posy < tab->baseh; posy++) {
        const int row = window ? STUDIO2VID_DISP_Y0 + posy : posy + orgy;
        const studio2vid_line *rec = &cap->lines[row];
        const int have = line_bytes(rec);

        if (!window && (tab->ovl & STUDIO2VID_OV_BANDS)) {
            if (row < STUDIO2VID_VIS_Y0)
                fill_rect(cnv, 0, posy * scale, tab->basew * scale, scale, 0xff808080, 70);
            if (band_int(row))
                fill_rect(cnv, 0, posy * scale, tab->basew * scale / 4, scale, 0xffff40d0, 150);
            if (band_efx(row))
                fill_rect(cnv, tab->basew * scale - tab->basew * scale / 4, posy * scale, tab->basew * scale / 4, scale, 0xff30d0ff, 150);
        }
        if ((tab->ovl & STUDIO2VID_OV_SHORT) && in_display(row) && have != 8) {
            fill_rect(cnv, 0, posy * scale, tab->basew * scale, scale, 0xffff9020, 55);
            fill_rect(cnv, 0, posy * scale, scale < 3 ? scale : 3, scale, 0xffff9020, 230);
        }
        if (!window && (tab->ovl & STUDIO2VID_OV_SOURCE) && in_display(row))
            for (int idx = 0; idx < have; idx++) {
                uint32_t on, off;
                src_tint(rec->src, &on, &off);
                fill_rect(cnv, (rec->hpos + BYTE_DX + idx * 8 - orgx) * scale, posy * scale, 8 * scale, scale, on, 70);
            }
        if (window && (tab->view == STUDIO2VID_V_VALUES) && (tab->ovl & STUDIO2VID_OV_SOURCE))
            for (int idx = 0; idx < have; idx++) {
                uint32_t on, off;
                src_tint(rec->src, &on, &off);
                fill_rect(cnv, idx * 8 * scale, posy * scale, 8 * scale, scale, on, 70);
            }
        if ((tab->ovl & STUDIO2VID_OV_GRID) && scale >= 2) {
            if (window) {
                for (int idx = 0; idx <= 8; idx++)
                    vline(cnv, idx * 8 * scale, posy * scale, (posy + 1) * scale, 0xffffffff, idx % 8 == 0 ? 120 : 70);
                if (posy % 4 == 0)
                    hline(cnv, posy * scale, 0, tab->basew * scale, 0xffffffff, 45);
            } else
                for (int idx = 0; idx <= have; idx++)
                    vline(cnv, (rec->hpos + BYTE_DX + idx * 8 - orgx) * scale, posy * scale, (posy + 1) * scale, 0xffffffff, 80);
            if (scale >= 8)
                for (int posx = 0; posx < tab->basew; posx++)
                    vline(cnv, posx * scale, posy * scale, (posy + 1) * scale, 0xffffffff, 25);
        }
    }
    if ((tab->ovl & STUDIO2VID_OV_ACTIVE) && !window) {
        int minx = RASTW, maxx = 0;
        for (int row = STUDIO2VID_DISP_Y0; row < STUDIO2VID_DISP_Y1; row++) {
            const studio2vid_line *rec = &cap->lines[row];
            const int have = line_bytes(rec);
            if (!have) continue;
            if (rec->hpos + BYTE_DX < minx) minx = rec->hpos + BYTE_DX;
            if (rec->hpos + BYTE_DX + have * 8 > maxx) maxx = rec->hpos + BYTE_DX + have * 8;
        }
        if (maxx > minx) {
            const int left = (minx - orgx) * scale, right = (maxx - orgx) * scale;
            const int top = (STUDIO2VID_DISP_Y0 - orgy) * scale, bottom = (STUDIO2VID_DISP_Y1 - orgy) * scale;
            hline(cnv, top, left, right, 0xffffffff, 170);
            hline(cnv, bottom - 1, left, right, 0xffffffff, 170);
            vline(cnv, left, top, bottom, 0xffffffff, 170);
            vline(cnv, right - 1, top, bottom, 0xffffffff, 170);
        }
    }
    if ((tab->ovl & STUDIO2VID_OV_BEAM) && tab->has_live && cap->beam_row < RASTH) {
        const int posy = window ? cap->beam_row - STUDIO2VID_DISP_Y0 : cap->beam_row - orgy;
        if (posy >= 0 && posy < tab->baseh) hline(cnv, posy * scale, 0, tab->basew * scale, 0xffff4040, 230);
    }
}

static int pick_zoom(const studio2vid_tab *tab)
{
    int scale = tab->zoom;
    if (scale == 0) {
        scale = 1;
        for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
            if (tab->basew * k_zooms[i] <= tab->vieww && tab->baseh * k_zooms[i] <= tab->viewh)
                scale = k_zooms[i];
    }
    while (scale > 1 && !fits_zoom(tab->basew, tab->baseh, scale)) {
        int lower = 1;
        for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
            if (k_zooms[i] < scale) lower = k_zooms[i];
        scale = lower;
    }
    return scale;
}

static void build_picture(studio2vid_tab *tab)
{
    int scale;
    canvas cnv;

    tab->why[0] = '\0';
    tab->picw = tab->pich = 0;
    if (!build_base(tab))
        return;
    scale = pick_zoom(tab);
    tab->effzoom = scale;
    tab->picw = tab->basew * scale;
    tab->pich = tab->baseh * scale;
    if (!ensure(&tab->pic, &tab->pic_cap, (size_t)tab->picw * (size_t)tab->pich)) {
        tab->picw = tab->pich = 0;
        why_set(tab, "Out of memory.");
        return;
    }
    if (scale == 1)
        memcpy(tab->pic, tab->base, (size_t)tab->basew * (size_t)tab->baseh * sizeof *tab->pic);
    else
        for (int posy = 0; posy < tab->pich; posy++) {
            const uint32_t *src = &tab->base[(size_t)(posy / scale) * (size_t)tab->basew];
            uint32_t *dst = &tab->pic[(size_t)posy * (size_t)tab->picw];
            for (int posx = 0; posx < tab->picw; posx++)
                dst[posx] = src[posx / scale];
        }
    cnv.pixp = tab->pic; cnv.wide = tab->picw; cnv.high = tab->pich;
    draw_overlays(tab, &cnv, scale);
}

/* ---- the inspector ----------------------------------------------------------------------- */

/* the byte of raster line `row` that drew raster pixel (rx, row): a burst that
 * ran off the end of a line carries on at the start of the next, so the
 * record of the line above is searched too. Returns 0 when none did. */
static int find_byte(const studio2vid_capture *cap, int rastx, int row, int *rec_row, int *index, int *bit)
{
    const long flat = (long)row * RASTW + rastx;
    for (int lineno = row; lineno >= 0 && lineno >= row - 1; lineno--) {
        const studio2vid_line *rec = &cap->lines[lineno];
        const int have = line_bytes(rec);
        for (int idx = 0; idx < have; idx++) {
            const long start = (long)lineno * RASTW + rec->hpos + BYTE_DX + idx * 8;
            if (flat >= start && flat < start + 8) {
                *rec_row = lineno; *index = idx; *bit = 7 - (int)(flat - start);
                return 1;
            }
        }
    }
    return 0;
}

static void locate(studio2vid_tab *tab, int posx, int posy, studio2vid_pixel *outp)
{
    const studio2vid_capture *cap = tab->cap;
    const int scale = tab->effzoom ? tab->effzoom : 1;
    int picx, picy, rastx = -1, row = -1, rec_row = -1, idx = -1, bit = -1, found = 0, num;
    const studio2vid_line *rec = NULL;

    memset(outp, 0, sizeof *outp);
    outp->line = -1; outp->column = -1; outp->index = -1; outp->bit = -1;
    if (!tab->picw || posx < 0 || posy < 0 || posx >= tab->picw || posy >= tab->pich)
        return;
    picx = posx / scale; picy = posy / scale;
    outp->valid = 1;
    outp->picx = picx; outp->picy = picy;
    outp->rgb = tab->base ? tab->base[(size_t)picy * (size_t)tab->basew + (size_t)picx] : COL_BLACK;

    if (tab->kind == BK_WINDOW) {
        row = STUDIO2VID_DISP_Y0 + picy;
        rec_row = row; idx = picx / 8; bit = 7 - picx % 8;
        rec = &cap->lines[row];
        found = idx < line_bytes(rec);
        if (found) rastx = rec->hpos + BYTE_DX + idx * 8 + (7 - bit);
    } else {
        rastx = picx + org_x(tab);
        row = picy + org_y(tab);
        found = find_byte(cap, rastx, row, &rec_row, &idx, &bit);
        if (found) rec = &cap->lines[rec_row];
    }
    outp->line = row;
    outp->column = rastx;
    if (found) {
        outp->dma = 1;
        outp->index = idx; outp->bit = bit;
        outp->addr = (unsigned)(rec->addr + idx) & 0xffffu;
        outp->value = rec->data[idx];
        outp->src = rec->src;
        outp->fresh = rec->fresh;
        outp->on = (rec->data[idx] >> bit) & 1;
    } else if (tab->kind != BK_WINDOW && rastx >= 0 && row >= 0 && row < RASTH && rastx < RASTW) {
        outp->on = cap->bits[row * RASTW + rastx];
    }
    outp->src_name = src_name(outp->src);

    num = snprintf(outp->text, sizeof outp->text, "Pixel %d,%d   line %d, clock %d\n", picx, picy, row, rastx);
    if (found) {
        num += snprintf(outp->text + num, sizeof outp->text - (size_t)num,
                      "DMA burst from line %d: %d byte%s, first from $%04X (%s), started at clock %d\n"
                      "Byte %d at $%04X = $%02X, bit %d = %d (%s)\n",
                      rec_row, rec->count, rec->count == 1 ? "" : "s", rec->addr, src_name(rec->src),
                      rec->hpos, idx, outp->addr, outp->value, bit, outp->on, outp->on ? "white" : "black");
        num += snprintf(outp->text + num, sizeof outp->text - (size_t)num, "%s%s",
                      rec->fresh ? "This frame" : "The previous frame's line",
                      rec->count != 8 ? " (short burst)" : "");
    } else if (tab->kind == BK_WINDOW) {
        num += snprintf(outp->text + num, sizeof outp->text - (size_t)num,
                      "No DMA byte here: line %d delivered %d byte%s", row, line_bytes(rec),
                      line_bytes(rec) == 1 ? "" : "s");
    } else {
        const char *why = !in_display(row) ? "outside the display lines (80-207)"
                        : !cap->disp ? "the display is off" : "no DMA byte covers this clock";
        num += snprintf(outp->text + num, sizeof outp->text - (size_t)num, "No DMA byte: %s\nBitmap pixel %s",
                      why, outp->on ? "white" : "black");
    }
    if (band_int(row) || band_efx(row))
        snprintf(outp->text + num, sizeof outp->text - (size_t)num, "\nLine %d: %s%s%s", row,
                 band_int(row) ? "INT asserted" : "", band_int(row) && band_efx(row) ? ", " : "",
                 band_efx(row) ? "EFx asserted" : "");
}

int studio2vid_tab_hover(studio2vid_tab *tab, int posx, int posy)
{
    char old[sizeof tab->hover.text];
    if (!tab) return 0;
    snprintf(old, sizeof old, "%s", tab->hover.valid ? tab->hover.text : "");
    tab->hovx = posx; tab->hovy = posy;
    locate(tab, posx, posy, &tab->hover);
    return strcmp(old, tab->hover.valid ? tab->hover.text : "") != 0;
}

void studio2vid_tab_pin(studio2vid_tab *tab, int posx, int posy)
{
    int scale;
    if (!tab) return;
    scale = tab->effzoom ? tab->effzoom : 1;
    if (tab->pinx >= 0 && posx / scale == tab->pinx / scale && posy / scale == tab->piny / scale) {
        studio2vid_tab_unpin(tab);
        return;
    }
    tab->pinx = posx; tab->piny = posy;
    locate(tab, posx, posy, &tab->pin);
    if (!tab->pin.valid) studio2vid_tab_unpin(tab);
}

void studio2vid_tab_unpin(studio2vid_tab *tab)
{
    if (!tab) return;
    tab->pinx = tab->piny = -1;
    memset(&tab->pin, 0, sizeof tab->pin);
}

const studio2vid_pixel *studio2vid_tab_pixel(const studio2vid_tab *tab)
{
    if (!tab) return NULL;
    if (tab->pin.valid) return &tab->pin;
    if (tab->hover.valid) return &tab->hover;
    return NULL;
}

int studio2vid_tab_marks(const studio2vid_tab *tab, int hover[4], int pin[4])
{
    int ret = 0, scale;
    if (!tab) return 0;
    scale = tab->effzoom ? tab->effzoom : 1;
    if (tab->hover.valid) {
        hover[0] = tab->hover.picx * scale; hover[1] = tab->hover.picy * scale; hover[2] = scale; hover[3] = scale;
        ret |= 1;
    }
    if (tab->pin.valid) {
        pin[0] = tab->pin.picx * scale; pin[1] = tab->pin.picy * scale; pin[2] = scale; pin[3] = scale;
        ret |= 2;
    }
    return ret;
}

int studio2vid_tab_can_break(const studio2vid_tab *tab)
{
    const studio2vid_pixel *pixp = studio2vid_tab_pixel(tab);
    return tab && tab->dbg && pixp && pixp->valid && pixp->dma && pixp->src == STUDIO2VID_SRC_RAM;
}

int studio2vid_tab_break_on_pixel(studio2vid_tab *tab)
{
    const studio2vid_pixel *pixp = studio2vid_tab_pixel(tab);
    studio2debug_breakpoint list[128];
    char cmd[32], out[256];
    int count;

    if (!studio2vid_tab_can_break(tab)) return -1;
    count = studio2debug_breakpoint_list(tab->dbg, list, 128);
    for (int i = 0; i < count; i++)
        if ((list[i].type & STUDIO2DEBUG_BP_WRITE) && list[i].start <= pixp->addr && list[i].end >= pixp->addr)
            return list[i].id;
    snprintf(cmd, sizeof cmd, "bpw $%04X", pixp->addr);
    out[0] = '\0';
    studio2debug_command(tab->dbg, cmd, out, sizeof out);
    count = studio2debug_breakpoint_list(tab->dbg, list, 128);
    for (int i = 0; i < count; i++)
        if ((list[i].type & STUDIO2DEBUG_BP_WRITE) && list[i].start <= pixp->addr && list[i].end >= pixp->addr)
            return list[i].id;
    return -1;
}

/* ---- the tables ---------------------------------------------------------------------------- */

static void reg_set(studio2vid_tab *tab, int *num, const char *name, const char *value,
                    const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void reg_set(studio2vid_tab *tab, int *num, const char *name, const char *value,
                    const char *fmt, ...)
{
    studio2vid_reg *reg;
    va_list args;
    int prev_age;
    if (*num >= MAX_REGS) return;
    reg = &tab->regs[*num];
    prev_age = reg->age;
    reg->name = name;
    snprintf(reg->value, sizeof reg->value, "%s", value);
    va_start(args, fmt);
    vsnprintf(reg->meaning, sizeof reg->meaning, fmt, args);
    va_end(args);
    if (!tab->regs_primed || strcmp(tab->prev_val[*num], reg->value) != 0)
        reg->age = tab->regs_primed ? 0 : 99;
    else
        reg->age = prev_age < 99 ? prev_age + 1 : 99;
    memcpy(tab->prev_val[*num], reg->value, sizeof tab->prev_val[*num]);
    (*num)++;
}

/* "line 80 clock 16, in 3 clocks" for an absolute timer time */
static void when_text(const studio2vid_capture *cap, uint64_t at, char *dst, size_t size)
{
    if (at == UINT64_MAX)
        snprintf(dst, size, "not armed");
    else
        snprintf(dst, size, "line %d clock %d, %s %llu clocks",
                 (int)((at / RASTW) % RASTH), (int)(at % RASTW), at >= cap->cycles ? "in" : "ago",
                 (unsigned long long)(at >= cap->cycles ? at - cap->cycles : cap->cycles - at));
}

static void build_tables(studio2vid_tab *tab)
{
    const studio2vid_capture *cap = tab->cap;
    int num = 0, shown = 0;
    char val[24], when[96];

    /* registers (ages carry over from the previous build) */
    reg_set(tab, &num, "DISP", cap->disp ? "on" : "off", "%s",
            cap->disp ? "INP 1 turned the display on; the DMA runs on lines 80-207"
                    : "display off (no DMA, a black picture) until the program reads INP 1");
    reg_set(tab, &num, "INT", cap->int_line ? "1" : "0", "%s",
            cap->int_line ? "asserted (lines 78-79): the CPU interrupts if IE is set"
                        : "released");
    reg_set(tab, &num, "EFx", cap->efx_line ? "1" : "0", "%s",
            cap->efx_line ? "EF1 high (lines 76-79 and 204-207): the program may skip the frame's wait"
                        : "EF1 low (the display window)");
    reg_set(tab, &num, "DMA OUT", cap->dma_line ? "1" : "0", "%s",
            cap->dma_line ? "requesting a byte now" : "idle (8 bytes on, 6 cycles off, once a line)");
    snprintf(val, sizeof val, "%04X", cap->r0);
    reg_set(tab, &num, "R0", val, "DMA pointer: the next byte's address (%s)",
            cap->r0 >= 0x0800 && !(cap->r0 & 0x0200) ? "RAM" : cap->r0 >= 0xF400 ? "cart raster" : "elsewhere");
    snprintf(val, sizeof val, "%04X", cap->last_dma_addr);
    reg_set(tab, &num, "LAST", val, "address of the last DMA byte fetched");
    snprintf(val, sizeof val, "%llu", (unsigned long long)cap->dma_bytes);
    reg_set(tab, &num, "BYTES", val, "DMA bytes fetched since power-on");
    snprintf(val, sizeof val, "%u", cap->inp1_count);
    reg_set(tab, &num, "INP1", val, "display-on strobes (INP 1) since power-on");
    snprintf(val, sizeof val, "%u", cap->short_lines);
    reg_set(tab, &num, "SHORT", val, "display lines of this frame with a short or missing burst");
    when_text(cap, cap->int_next, when, sizeof when);
    snprintf(val, sizeof val, "%d", (int)((cap->int_next / RASTW) % RASTH));
    reg_set(tab, &num, "INT@", val, "next INT edge: %s", when);
    when_text(cap, cap->efx_next, when, sizeof when);
    snprintf(val, sizeof val, "%d", (int)((cap->efx_next / RASTW) % RASTH));
    reg_set(tab, &num, "EFx@", val, "next EFx edge: %s", when);
    when_text(cap, cap->dma_next, when, sizeof when);
    snprintf(val, sizeof val, "%d", (int)((cap->dma_next / RASTW) % RASTH));
    reg_set(tab, &num, "DMA@", val, "next DMA edge: %s", when);
    snprintf(val, sizeof val, "P%u IE%u", cap->cpu_p, cap->cpu_ie);
    reg_set(tab, &num, "CPU", val, "%s", cap->cpu_p == 1 ? "in the interrupt routine (P = 1)" : "running the program");
    tab->nregs = num;
    tab->regs_primed = 1;

    /* colours: bits of the display window's DMA bytes */
    tab->uses[0] = tab->uses[1] = 0;
    for (int row = STUDIO2VID_DISP_Y0; row < STUDIO2VID_DISP_Y1; row++) {
        const studio2vid_line *rec = &cap->lines[row];
        for (int idx = 0; idx < line_bytes(rec); idx++)
            for (int bit = 0; bit < 8; bit++)
                tab->uses[(rec->data[idx] >> bit) & 1]++;
    }
    tab->swat[0].index = 0; tab->swat[0].name = "Black (0)"; tab->swat[0].rgb = COL_BLACK; tab->swat[0].uses = tab->uses[0];
    tab->swat[1].index = 1; tab->swat[1].name = "White (1)"; tab->swat[1].rgb = COL_WHITE; tab->swat[1].uses = tab->uses[1];

    /* lines: every record, and every display line */
    tab->nrows = 0;
    for (int row = 0; row < RASTH; row++) {
        const studio2vid_line *rec = &cap->lines[row];
        const studio2vid_line_row *prev = tab->nrows ? &tab->rows[tab->nrows - 1] : NULL;
        studio2vid_line_row *out;
        int mask = 0;
        if (!rec->valid && !in_display(row)) continue;
        out = &tab->rows[tab->nrows];
        memset(out, 0, sizeof *out);
        out->row = row;
        out->display = in_display(row);
        out->valid = rec->valid;
        out->fresh = rec->fresh;
        out->count = rec->valid ? rec->count : 0;
        out->src = rec->src;
        out->src_name = src_name(rec->src);
        out->hpos = rec->hpos;
        out->addr = rec->addr;
        memcpy(out->data, rec->data, sizeof out->data);
        out->flags = rec->flags;
        if (prev) {
            if (out->addr != ((prev->addr + (unsigned)(prev->count > 8 ? 8 : prev->count)) & 0xffffu)) mask |= STUDIO2VID_LC_ADDR;
            if (out->count != prev->count) mask |= STUDIO2VID_LC_COUNT;
            if (out->hpos != prev->hpos) mask |= STUDIO2VID_LC_HPOS;
            if (out->src != prev->src) mask |= STUDIO2VID_LC_SRC;
            if (out->flags != prev->flags) mask |= STUDIO2VID_LC_FLAGS;
        } else {
            mask = STUDIO2VID_LC_ADDR;
        }
        out->changed_mask = mask;
        tab->nrows++;
    }
    for (int row = STUDIO2VID_DISP_Y0; row < STUDIO2VID_DISP_Y1; row++)
        shown += cap->lines[row].valid && cap->lines[row].count == 8;

    /* timing */
    tab->ntiming = 0;
#define TLINE(...) do { if (tab->ntiming < MAX_TIMING) { \
        snprintf(tab->timing_buf[tab->ntiming], sizeof tab->timing_buf[0], __VA_ARGS__); \
        tab->timing_ptr[tab->ntiming] = tab->timing_buf[tab->ntiming]; tab->ntiming++; } } while (0)
    TLINE("Beam\tline %d of %d, clock %d", cap->vpos, RASTH, cap->hpos);
    TLINE("Frame\t%llu", (unsigned long long)cap->frame);
    TLINE("Raster\t%d clocks a line, %d lines, %d clocks a frame", RASTW, RASTH, RASTW * RASTH);
    TLINE("Rate\t%.2f Hz at 1.76 MHz", 1760000.0 / (double)(RASTW * RASTH));
    TLINE("Display lines\t%d-%d (%d of %d full 8-byte bursts)", STUDIO2VID_DISP_Y0, STUDIO2VID_DISP_Y1 - 1, shown, WIN_H);
    TLINE("INT\tlines %d-%d%s", STUDIO2VID_INT_Y0, STUDIO2VID_DISP_Y0 - 1, cap->int_line ? ", asserted now" : "");
    TLINE("EFx\tlines %d-%d and %d-%d%s", STUDIO2VID_EFX_TOP_Y0, STUDIO2VID_DISP_Y0 - 1,
       STUDIO2VID_EFX_BOT_Y0, STUDIO2VID_DISP_Y1 - 1, cap->efx_line ? ", high now" : "");
    TLINE("Short bursts\t%u display line%s", cap->short_lines, cap->short_lines == 1 ? "" : "s");
    TLINE("Rows listed\t%d", tab->nrows);
    TLINE("Machine\t%s", tab->stopped ? "stopped" : "running");
#undef TLINE

    snprintf(tab->status, sizeof tab->status,
             "%s view, display %s, %u short line%s in the frame\n%s%s",
             k_view_names[tab->view], cap->disp && !cap->blank ? "on" : "off", cap->short_lines,
             cap->short_lines == 1 ? "" : "s",
             tab->stopped ? "Stopped" : "Running: showing the last finished frame",
             tab->stopped ? (tab->has_fb ? "" : " (no frame has finished yet)") : "");
}

/* ---- refresh -------------------------------------------------------------------------------- */

unsigned studio2vid_tab_refresh(studio2vid_tab *tab, int force)
{
    uint64_t sig;
    unsigned ret = 0;
    int fresh;

    if (!tab) return 0;
    if (!acquire(tab)) {
        why_set(tab, "The machine is not running.");
        tab->picw = tab->pich = 0;
        return STUDIO2VID_TAB_OFF | STUDIO2VID_TAB_PICTURE | STUDIO2VID_TAB_TABLES;
    }
    sig = signature(tab);
    fresh = force || sig != tab->sig;
    if (fresh)
        tab->sig = sig;
    if (fresh || tab->dirty) {
        build_picture(tab);
        tab->dirty = 0;
        ret |= STUDIO2VID_TAB_PICTURE;
    }
    if (fresh) {
        if (tab->hovx >= 0) locate(tab, tab->hovx, tab->hovy, &tab->hover);
        if (tab->pinx >= 0) locate(tab, tab->pinx, tab->piny, &tab->pin);
        build_tables(tab);
        ret |= STUDIO2VID_TAB_TABLES;
    }
    return ret;
}

const uint32_t *studio2vid_tab_picture(studio2vid_tab *tab, int *wide, int *high)
{
    if (!tab || !tab->picw) return NULL;
    if (wide) *wide = tab->picw;
    if (high) *high = tab->pich;
    return tab->pic;
}
const char *studio2vid_tab_why(const studio2vid_tab *tab) { return tab ? tab->why : ""; }
int studio2vid_tab_effective_zoom(const studio2vid_tab *tab) { return tab ? tab->effzoom : 1; }
const char *studio2vid_tab_status(const studio2vid_tab *tab) { return tab ? tab->status : ""; }
int studio2vid_tab_regs(const studio2vid_tab *tab, const studio2vid_reg **out)
{
    if (out) *out = tab->regs;
    return tab->nregs;
}
int studio2vid_tab_swatches(const studio2vid_tab *tab, const studio2vid_swatch **out)
{
    /* the inspected pixel moves between refreshes */
    const studio2vid_pixel *pixp = studio2vid_tab_pixel(tab);
    studio2vid_swatch *swat = ((studio2vid_tab *)tab)->swat;
    for (int i = 0; i < 2; i++)
        swat[i].inspected = pixp && pixp->valid && pixp->on == i;
    if (out) *out = tab->swat;
    return 2;
}
int studio2vid_tab_lines(const studio2vid_tab *tab, const studio2vid_line_row **out)
{
    if (out) *out = tab->rows;
    return tab->nrows;
}
int studio2vid_tab_timing(const studio2vid_tab *tab, const char *const **lines)
{
    if (lines) *lines = (const char *const *)tab->timing_ptr;
    return tab->ntiming;
}
int studio2vid_tab_beam_permille(const studio2vid_tab *tab)
{
    return tab && tab->have ? tab->cap->vpos * 1000 / RASTH : 0;
}

int studio2vid_tab_text(studio2vid_tab *tab, int section, char *dst, int dstsz)
{
    int num = 0;
#define PUT(...) do { if (num < dstsz) num += snprintf(dst + num, (size_t)(dstsz - num), __VA_ARGS__); } while (0)
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!tab || !tab->have) { PUT("The machine is not running.\n"); return num < dstsz ? num : dstsz - 1; }
    switch (section) {
    case STUDIO2VID_S_REGISTERS:
        for (int i = 0; i < tab->nregs; i++)
            PUT("%-8s %-8s %s\n", tab->regs[i].name, tab->regs[i].value, tab->regs[i].meaning);
        break;
    case STUDIO2VID_S_COLOURS:
        for (int i = 0; i < 2; i++)
            PUT("%-10s #%06X  %ld bits\n", tab->swat[i].name, tab->swat[i].rgb & 0xffffff, tab->swat[i].uses);
        break;
    case STUDIO2VID_S_LINES:
        for (int i = 0; i < tab->nrows; i++) {
            const studio2vid_line_row *ret = &tab->rows[i];
            PUT("line %3d  ", ret->row);
            if (!ret->valid) {
                PUT("no DMA\n");
                continue;
            }
            PUT("$%04X x%d clock %3d %-11s %02X %02X %02X %02X %02X %02X %02X %02X  %s%s%s%s%s%s\n", ret->addr, ret->count,
              ret->hpos, ret->src_name, ret->data[0], ret->data[1], ret->data[2], ret->data[3], ret->data[4], ret->data[5],
              ret->data[6], ret->data[7], ret->fresh ? "" : "old ", ret->count != 8 ? "short " : "",
              (ret->flags & STUDIO2VID_LF_INT) ? "INT " : "", (ret->flags & STUDIO2VID_LF_EFX) ? "EFx " : "",
              (ret->changed_mask & STUDIO2VID_LC_ADDR) ? "R0-jump " : "", (ret->flags & STUDIO2VID_LF_DISP) ? "" : "off");
        }
        break;
    case STUDIO2VID_S_TIMING:
        for (int i = 0; i < tab->ntiming; i++) PUT("%s\n", tab->timing_buf[i]);
        break;
    default:
        PUT("%s\n", tab->status);
        if (studio2vid_tab_pixel(tab)) PUT("%s\n", studio2vid_tab_pixel(tab)->text);
        break;
    }
#undef PUT
    return num < dstsz ? num : dstsz - 1;
}
