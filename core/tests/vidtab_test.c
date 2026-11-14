/*
 * vidtab_test -- the Video tab's controller (core/debugger/vidtab.c) on
 * synthetic frames: no machine runs, the test feeds the captures a machine
 * would take. Views and their sizes, zoom and Fit, the Decoded redraw from
 * the DMA bytes and its source tints, a frame split by a stop (this frame
 * above the beam, the last below), short bursts, the inspector's byte
 * lookup, the tables, the text sections, and a fuzz over random captures
 * that must never crash or return a picture of the wrong size.
 *
 * No setenv/unsetenv or POSIX-only calls: this builds under MinGW too.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "studio2vidcap.h"
#include "studio2vidtab.h"

#define WHITE 0xffffffffu
#define BLACK 0xff000000u

static int failures;
static void check(int good, const char *what)
{
    printf("%s: %s\n", good ? "ok" : "FAIL", what);
    if (!good) failures++;
}

static studio2vid_capture frame_a, frame_b, scratch;

/* One line's DMA burst, and its pixels in the raster. */
static void put_line(studio2vid_capture *cap, int row, unsigned addr, int count, int hpos, int src,
                     int fresh, const uint8_t *data)
{
    studio2vid_line *rec = &cap->lines[row];
    memset(rec, 0, sizeof *rec);
    rec->valid = 1;
    rec->fresh = (uint8_t)fresh;
    rec->count = (uint8_t)count;
    rec->src = (uint8_t)src;
    rec->hpos = (uint8_t)hpos;
    rec->addr = (uint16_t)addr;
    rec->flags = STUDIO2VID_LF_DISP;
    for (int idx = 0; idx < 8 && idx < count; idx++) {
        rec->data[idx] = data[idx];
        for (int bit = 0; bit < 8; bit++) {
            const long flat = (long)row * STUDIO2VID_RASTER_W + hpos + 4 + idx * 8 + bit;
            if (flat >= 0 && flat < (long)sizeof cap->bits)
                cap->bits[flat] = (data[idx] >> (7 - bit)) & 1;
        }
    }
}

/* The BIOS's picture: 32 rows of 8 bytes at $0900, each shown four lines running. */
static void bios_frame(studio2vid_capture *cap, uint8_t seed, int fresh, int beam)
{
    memset(cap, 0, sizeof *cap);
    cap->disp = 1;
    cap->serial = 1;
    cap->frame = 5;
    cap->beam_row = beam;
    cap->vpos = beam < STUDIO2VID_RASTER_H ? beam : 0;
    cap->r0 = 0x0900;
    for (int line = 0; line < 128; line++) {
        uint8_t data[8];
        const unsigned rowaddr = 0x0900 + (unsigned)(line / 4) * 8;
        for (int idx = 0; idx < 8; idx++)
            data[idx] = (uint8_t)(seed + (line / 4) * 8 + idx * 37);
        put_line(cap, STUDIO2VID_DISP_Y0 + line, rowaddr, 8, 12, STUDIO2VID_SRC_RAM, fresh, data);
    }
}

static uint32_t pixel_at(const uint32_t *pic, int wide, int col, int row)
{
    return pic[(size_t)row * (size_t)wide + (size_t)col];
}

static const int k_fuzz_zoom[6] = { 0, 1, 2, 3, 8, 16 };

static uint32_t rng_state = 12345;
static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

int main(void)
{
    studio2vid_tab *tab = studio2vid_tab_new(NULL);
    const uint32_t *pic;
    int wide, high;

    check(tab != NULL, "tab");
    check(studio2vid_tab_break_on_pixel(tab) == -1 && !studio2vid_tab_can_break(tab),
          "no debugger: Break on writes refuses");

    /* ---- Display ---- */
    bios_frame(&frame_a, 0x5a, 1, STUDIO2VID_RASTER_H);
    studio2vid_tab_feed(tab, &frame_a, 0);
    studio2vid_tab_set_zoom(tab, 1);
    studio2vid_tab_set_overlays(tab, 0);
    check((studio2vid_tab_refresh(tab, 1) & STUDIO2VID_TAB_PICTURE) != 0, "refresh");
    pic = studio2vid_tab_picture(tab, &wide, &high);
    check(pic && wide == 100 && high == 246, "Display is the 100 x 246 visible area");
    {
        int wrong = 0;
        for (int row = 0; pic && row < high; row++)
            for (int col = 0; col < wide; col++)
                wrong += pixel_at(pic, wide, col, row) !=
                         (frame_a.bits[(row + 16) * 112 + col + 12] ? WHITE : BLACK);
        check(wrong == 0, "Display pixels are the raster's visible bits");
    }
    frame_a.disp = 0;
    studio2vid_tab_feed(tab, &frame_a, 0);
    studio2vid_tab_refresh(tab, 1);
    pic = studio2vid_tab_picture(tab, &wide, &high);
    {
        int lit = 0;
        for (int pix = 0; pic && pix < wide * high; pix++) lit += pic[pix] != BLACK;
        check(pic && lit == 0, "display off: a black picture");
    }
    bios_frame(&frame_a, 0x5a, 1, STUDIO2VID_RASTER_H);
    frame_a.blank = 1;
    studio2vid_tab_feed(tab, &frame_a, 0);
    studio2vid_tab_refresh(tab, 1);
    pic = studio2vid_tab_picture(tab, &wide, &high);
    {
        int lit = 0;
        for (int pix = 0; pic && pix < wide * high; pix++) lit += pic[pix] != BLACK;
        check(pic && lit == 0, "the stand-in parked: a black picture");
    }
    bios_frame(&frame_a, 0x5a, 1, STUDIO2VID_RASTER_H);

    /* ---- Decoded and Values ---- */
    studio2vid_tab_feed(tab, &frame_a, 0);
    studio2vid_tab_set_view(tab, STUDIO2VID_V_DECODED);
    studio2vid_tab_refresh(tab, 1);
    pic = studio2vid_tab_picture(tab, &wide, &high);
    check(pic && wide == 64 && high == 128, "Decoded is the 64 x 128 display window");
    {
        int wrong = 0;
        for (int line = 0; pic && line < 128; line++)
            for (int col = 0; col < 64; col++) {
                const int bit = (frame_a.lines[80 + line].data[col / 8] >> (7 - col % 8)) & 1;
                wrong += pixel_at(pic, wide, col, line) != (bit ? 0xffd8f0d8u : 0xff0c1a0cu);
            }
        check(wrong == 0, "Decoded redraws every DMA byte in the RAM tint");
    }
    studio2vid_tab_set_view(tab, STUDIO2VID_V_VALUES);
    studio2vid_tab_refresh(tab, 1);
    pic = studio2vid_tab_picture(tab, &wide, &high);
    check(pic && wide == 64 && high == 128, "Values is 64 x 128");
    check(pic && pixel_at(pic, wide, 0, 0) == pixel_at(pic, wide, 7, 0) && pixel_at(pic, wide, 0, 0) != pixel_at(pic, wide, 8, 0),
          "Values: a byte is one flat colour, the next byte another");

    /* the source tints differ by page */
    {
        const int kinds[] = { STUDIO2VID_SRC_RAM, STUDIO2VID_SRC_RASTER, STUDIO2VID_SRC_CART,
                              STUDIO2VID_SRC_BIOS, STUDIO2VID_SRC_OPEN };
        uint32_t seen[5];
        for (int kind = 0; kind < 5; kind++) {
            const uint8_t ones[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
            bios_frame(&scratch, 0, 1, STUDIO2VID_RASTER_H);
            put_line(&scratch, 80, 0x0900, 8, 12, kinds[kind], 1, ones);
            studio2vid_tab_feed(tab, &scratch, 0);
            studio2vid_tab_set_view(tab, STUDIO2VID_V_DECODED);
            studio2vid_tab_refresh(tab, 1);
            pic = studio2vid_tab_picture(tab, &wide, &high);
            seen[kind] = pic ? pixel_at(pic, wide, 0, 0) : 0;
        }
        {
            int distinct = 1;
            for (int one = 0; one < 5; one++)
                for (int two = one + 1; two < 5; two++)
                    distinct &= seen[one] != seen[two];
            check(distinct, "each kind of source page has its own tint");
        }
    }

    /* ---- sizes: zoom, Fit ---- */
    bios_frame(&frame_a, 0x11, 1, STUDIO2VID_RASTER_H);
    studio2vid_tab_feed(tab, &frame_a, 0);
    studio2vid_tab_set_view(tab, STUDIO2VID_V_DISPLAY);
    studio2vid_tab_set_zoom(tab, 3);
    studio2vid_tab_refresh(tab, 1);
    pic = studio2vid_tab_picture(tab, &wide, &high);
    check(pic && wide == 300 && high == 738 && studio2vid_tab_effective_zoom(tab) == 3, "zoom 3");
    studio2vid_tab_set_zoom(tab, 0);
    studio2vid_tab_set_viewport(tab, 400, 800);
    studio2vid_tab_refresh(tab, 1);
    pic = studio2vid_tab_picture(tab, &wide, &high);
    check(pic && studio2vid_tab_effective_zoom(tab) == 3 && high == 738, "Fit: the height decides (800 high -> 3x)");
    studio2vid_tab_set_viewport(tab, 150, 5000);
    studio2vid_tab_refresh(tab, 1);
    check(studio2vid_tab_effective_zoom(tab) == 1, "Fit: the width limits too (150 wide -> 1x)");
    studio2vid_tab_set_viewport(tab, 10, 10);
    studio2vid_tab_refresh(tab, 1);
    check(studio2vid_tab_effective_zoom(tab) == 1, "Fit never goes below 1x");
    studio2vid_tab_set_zoom(tab, 16);
    studio2vid_tab_refresh(tab, 1);
    pic = studio2vid_tab_picture(tab, &wide, &high);
    check(pic && studio2vid_tab_effective_zoom(tab) == 12 && wide == 1200 && high == 2952,
          "zoom 16 of the tall Display is held to the picture-size cap (12x)");
    studio2vid_tab_set_view(tab, STUDIO2VID_V_DECODED);
    studio2vid_tab_refresh(tab, 1);
    pic = studio2vid_tab_picture(tab, &wide, &high);
    check(pic && wide == 1024 && high == 2048, "zoom 16 of Decoded");
    studio2vid_tab_set_view(tab, STUDIO2VID_V_DISPLAY);
    studio2vid_tab_set_zoom(tab, 1);

    /* ---- a frame split by a stop: this frame above the beam, the last below ---- */
    bios_frame(&frame_a, 0x11, 1, STUDIO2VID_RASTER_H);        /* the finished frame */
    bios_frame(&frame_b, 0xc3, 1, 150);                         /* live to line 150 ... */
    for (int row = 150; row < STUDIO2VID_RASTER_H; row++) {     /* ... the old frame below */
        frame_b.lines[row] = frame_a.lines[row];
        frame_b.lines[row].fresh = 0;
        memcpy(&frame_b.bits[row * STUDIO2VID_RASTER_W], &frame_a.bits[row * STUDIO2VID_RASTER_W],
               STUDIO2VID_RASTER_W);
    }
    studio2vid_tab_feed(tab, &frame_b, 1);
    studio2vid_tab_set_view(tab, STUDIO2VID_V_BEAM);
    studio2vid_tab_set_overlays(tab, 0);
    studio2vid_tab_refresh(tab, 1);
    pic = studio2vid_tab_picture(tab, &wide, &high);
    check(pic && wide == 112 && high == 262, "Beam is the whole 112 x 262 raster");
    {
        int above_wrong = 0, below_dim = 0, below_lit = 0;
        for (int row = 0; pic && row < 262; row++)
            for (int col = 0; col < 112; col++) {
                const uint32_t val = pixel_at(pic, wide, col, row);
                if (row < 150) above_wrong += val != (frame_b.bits[row * 112 + col] ? WHITE : BLACK);
                else if (frame_b.bits[row * 112 + col]) {
                    below_lit++;
                    below_dim += val != WHITE && (val & 0xff) < 0xff;
                }
            }
        check(above_wrong == 0, "Beam: rows above the beam are bright");
        check(below_lit > 100 && below_dim == below_lit, "Beam: lit pixels below the beam are dimmed");
    }
    studio2vid_tab_set_overlays(tab, STUDIO2VID_OV_BEAM);
    studio2vid_tab_refresh(tab, 1);
    pic = studio2vid_tab_picture(tab, &wide, &high);
    {
        const uint32_t val = pic ? pixel_at(pic, wide, 5, 150) : 0;
        check(((val >> 16) & 255) > 180 && ((val >> 8) & 255) < 120, "the Beam overlay draws a red line at the beam");
    }
    studio2vid_tab_set_overlays(tab, 0);

    /* Decoded of the split frame: live bytes above the beam, the last frame's below */
    studio2vid_tab_set_view(tab, STUDIO2VID_V_DECODED);
    studio2vid_tab_refresh(tab, 1);
    pic = studio2vid_tab_picture(tab, &wide, &high);
    {
        int wrong = 0;
        for (int line = 0; pic && line < 128; line++) {
            const studio2vid_line *rec = &frame_b.lines[80 + line];
            for (int col = 0; col < 64; col++) {
                const int bit = (rec->data[col / 8] >> (7 - col % 8)) & 1;
                wrong += pixel_at(pic, wide, col, line) != (bit ? 0xffd8f0d8u : 0xff0c1a0cu);
            }
        }
        check(wrong == 0 && frame_b.lines[100].data[0] != frame_b.lines[200].data[0],
              "Decoded follows each line's own record across the split");
    }

    /* the Beam view needs a stopped machine */
    studio2vid_tab_feed(tab, &frame_a, 0);
    studio2vid_tab_set_view(tab, STUDIO2VID_V_BEAM);
    studio2vid_tab_refresh(tab, 1);
    check(studio2vid_tab_picture(tab, &wide, &high) == NULL && strlen(studio2vid_tab_why(tab)) > 20,
          "Beam while running says why");
    studio2vid_tab_set_view(tab, STUDIO2VID_V_DISPLAY);

    /* ---- the inspector ---- */
    studio2vid_tab_feed(tab, &frame_a, 0);
    studio2vid_tab_set_zoom(tab, 2);
    studio2vid_tab_set_overlays(tab, 0);
    studio2vid_tab_refresh(tab, 1);
    {
        /* raster (12 + 4 + 8*3 + 2, 80 + 9): byte 3 of line 89, bit 5 */
        const int colx = 16 + 24 + 2 - 12, rowy = 89 - 16;
        const studio2vid_pixel *pix;
        const studio2vid_line *rec = &frame_a.lines[89];
        studio2vid_tab_hover(tab, colx * 2, rowy * 2);
        pix = studio2vid_tab_pixel(tab);
        check(pix && pix->valid && pix->dma && pix->line == 89 && pix->index == 3 && pix->bit == 5,
              "Display hover finds the line, byte and bit");
        check(pix && pix->addr == rec->addr + 3u && pix->value == rec->data[3] && pix->src == STUDIO2VID_SRC_RAM,
              "...and the byte's address, value and source");
        check(pix && pix->on == ((rec->data[3] >> 5) & 1), "...and the pixel's own colour");
        check(pix && strstr(pix->text, "$0913") && strstr(pix->text, "RAM"), "the inspector text names the address and page");
        check(studio2vid_tab_can_break(tab) == 0, "no debugger: cannot break");
        studio2vid_tab_pin(tab, colx * 2, rowy * 2);
        studio2vid_tab_hover(tab, 0, 0);
        check(studio2vid_tab_pixel(tab) && studio2vid_tab_pixel(tab)->index == 3, "a pin outlives the hover");
        {
            int hov[4], pinbox[4];
            const int marks = studio2vid_tab_marks(tab, hov, pinbox);
            check(marks == 3 && pinbox[0] == colx * 2 && pinbox[2] == 2, "marks: hover and pin boxes at the zoom");
        }
        studio2vid_tab_pin(tab, colx * 2, rowy * 2);
        studio2vid_tab_hover(tab, -1, -1);
        check(studio2vid_tab_pixel(tab) == NULL, "the same spot unpins; a hover that left clears");
    }
    studio2vid_tab_set_view(tab, STUDIO2VID_V_DECODED);
    studio2vid_tab_set_zoom(tab, 1);
    studio2vid_tab_refresh(tab, 1);
    {
        const studio2vid_pixel *pix;
        studio2vid_tab_hover(tab, 8 * 5 + 3, 40);
        pix = studio2vid_tab_pixel(tab);
        check(pix && pix->dma && pix->line == 120 && pix->index == 5 && pix->bit == 4 &&
              pix->addr == frame_a.lines[120].addr + 5u, "Decoded hover: line 80+y, byte x/8, bit 7-x%8");
    }
    studio2vid_tab_set_view(tab, STUDIO2VID_V_BEAM);
    studio2vid_tab_feed(tab, &frame_b, 1);
    studio2vid_tab_refresh(tab, 1);
    {
        const studio2vid_pixel *pix;
        studio2vid_tab_hover(tab, 12 + 4 + 8 * 2 + 1, 100);
        pix = studio2vid_tab_pixel(tab);
        check(pix && pix->dma && pix->fresh && pix->index == 2, "Beam hover: a live line is 'this frame'");
        studio2vid_tab_hover(tab, 12 + 4 + 8 * 2 + 1, 200);
        pix = studio2vid_tab_pixel(tab);
        check(pix && pix->dma && !pix->fresh && strstr(pix->text, "previous frame"), "...a line below the beam is the previous frame's");
        studio2vid_tab_hover(tab, 3, 30);
        pix = studio2vid_tab_pixel(tab);
        check(pix && pix->valid && !pix->dma && strstr(pix->text, "outside the display lines"),
              "a pixel outside the display lines has no DMA byte");
        studio2vid_tab_hover(tab, 3, 78);
        pix = studio2vid_tab_pixel(tab);
        check(pix && strstr(pix->text, "INT asserted"), "line 78 says INT is asserted");
        studio2vid_tab_hover(tab, 3, 205);
        pix = studio2vid_tab_pixel(tab);
        check(pix && strstr(pix->text, "EFx asserted"), "line 205 says EFx is asserted");
        studio2vid_tab_hover(tab, -1, -1);
    }

    /* ---- short bursts ---- */
    bios_frame(&frame_a, 0x77, 1, STUDIO2VID_RASTER_H);
    {
        const uint8_t part[8] = { 1, 2, 3, 4, 5, 0, 0, 0 };
        memset(&frame_a.bits[100 * 112], 0, 112);
        put_line(&frame_a, 100, 0x0A00, 5, 12, STUDIO2VID_SRC_OPEN, 1, part);
        memset(&frame_a.lines[120], 0, sizeof frame_a.lines[120]);   /* no DMA at all */
        frame_a.short_lines = 2;
    }
    studio2vid_tab_feed(tab, &frame_a, 0);
    studio2vid_tab_set_view(tab, STUDIO2VID_V_DECODED);
    studio2vid_tab_set_overlays(tab, STUDIO2VID_OV_SHORT);
    studio2vid_tab_set_zoom(tab, 1);
    studio2vid_tab_refresh(tab, 1);
    pic = studio2vid_tab_picture(tab, &wide, &high);
    check(pic && pixel_at(pic, wide, 45, 20) != pixel_at(pic, wide, 5, 20) && (pixel_at(pic, wide, 45, 20) & 0xffffff) > 0 &&
          pixel_at(pic, wide, 63, 20) == pixel_at(pic, wide, 40, 20), "a short burst: the missing bytes are drawn as missing");
    check(pic && ((pixel_at(pic, wide, 0, 20) >> 16) & 255) > 100, "the Short overlay marks the short line");
    {
        const studio2vid_line_row *rows;
        const int nrows = studio2vid_tab_lines(tab, &rows);
        int display = 0, found = 0, jumped = 0;
        for (int i = 0; i < nrows; i++) {
            display += rows[i].display;
            if (rows[i].row == 100) found = rows[i].count == 5 && rows[i].src == STUDIO2VID_SRC_OPEN &&
                                            (rows[i].changed_mask & STUDIO2VID_LC_COUNT);
            if (rows[i].row == 121) jumped = (rows[i].changed_mask & STUDIO2VID_LC_ADDR) != 0;
        }
        check(nrows == 128 && display == 128, "Lines lists every display line, record or none");
        check(found, "Lines: the short burst has its count and source");
        check(jumped, "Lines: an R0 reload is flagged");
    }
    {
        const studio2vid_reg *regs;
        int nregs = studio2vid_tab_regs(tab, &regs), have_short = 0, have_disp = 0;
        for (int i = 0; i < nregs; i++) {
            have_short |= !strcmp(regs[i].name, "SHORT") && !strcmp(regs[i].value, "2");
            have_disp |= !strcmp(regs[i].name, "DISP") && !strcmp(regs[i].value, "on");
        }
        check(have_short && have_disp, "Registers: DISP on, SHORT 2");
    }
    {
        const studio2vid_swatch *swat;
        long total = 0;
        check(studio2vid_tab_swatches(tab, &swat) == 2, "two colours");
        total = swat[0].uses + swat[1].uses;
        check(total == 8L * (126 * 8 + 5) && swat[1].uses > 0 && swat[0].rgb == BLACK && swat[1].rgb == WHITE,
              "Colours: every delivered bit counted once");
    }

    /* ---- the text sections ---- */
    {
        char text[16384];
        for (int sec = 0; sec < STUDIO2VID_S_COUNT; sec++) {
            const int len = studio2vid_tab_text(tab, sec, text, sizeof text);
            check(len > 5 && len == (int)strlen(text), studio2vid_section_name(sec));
        }
        studio2vid_tab_text(tab, STUDIO2VID_S_TIMING, text, sizeof text);
        check(strstr(text, "Beam") && strstr(text, "59.98") && strstr(text, "Short bursts"), "timing text");
        studio2vid_tab_text(tab, STUDIO2VID_S_LINES, text, sizeof text);
        check(strstr(text, "line 100") && strstr(text, "short") && strstr(text, "no DMA"), "lines text");
        check(studio2vid_tab_text(tab, STUDIO2VID_S_LINES, text, 40) <= 39, "a small buffer is respected");
        check(studio2vid_section_from_name("reg") == STUDIO2VID_S_REGISTERS && studio2vid_section_from_name("zzz") == -1,
              "section names by prefix");
    }
    {
        const char *const *timing;
        check(studio2vid_tab_timing(tab, &timing) >= 8, "timing lines");
        check(strchr(studio2vid_tab_status(tab), '\n') != NULL, "status is two lines");
    }
    check(studio2vid_tab_beam_permille(tab) >= 0, "beam permille");

    /* ---- fuzz: random captures, views, zooms, hovers ---- */
    for (int round = 0; round < 300; round++) {
        memset(&scratch, 0, sizeof scratch);
        scratch.serial = 1 + (uint64_t)round;
        scratch.disp = (int)(rnd() & 1);
        scratch.beam_row = (int)(rnd() % 263);
        scratch.vpos = (int)(rnd() % 262);
        scratch.hpos = (int)(rnd() % 112);
        for (int pix = 0; pix < (int)sizeof scratch.bits; pix++) scratch.bits[pix] = (uint8_t)(rnd() & 1);
        for (int row = 0; row < STUDIO2VID_RASTER_H; row++) {
            studio2vid_line *rec = &scratch.lines[row];
            rec->valid = (uint8_t)(rnd() % 4 != 0);
            rec->fresh = (uint8_t)(rnd() & 1);
            rec->count = (uint8_t)(rnd() % 12);
            rec->src = (uint8_t)(rnd() % (STUDIO2VID_SRC_COUNT + 2));
            rec->hpos = (uint8_t)(rnd() % 112);
            rec->addr = (uint16_t)rnd();
            rec->flags = (uint8_t)rnd();
            for (int idx = 0; idx < 8; idx++) rec->data[idx] = (uint8_t)rnd();
        }
        studio2vid_tab_feed(tab, &scratch, (int)(rnd() & 1));
        studio2vid_tab_set_view(tab, (int)(rnd() % STUDIO2VID_V_COUNT));
        studio2vid_tab_set_overlays(tab, rnd() & STUDIO2VID_OV_ALL);
        studio2vid_tab_set_zoom(tab, k_fuzz_zoom[rnd() % 6]);
        studio2vid_tab_refresh(tab, 1);
        pic = studio2vid_tab_picture(tab, &wide, &high);
        if (pic) {
            const int zoom = studio2vid_tab_effective_zoom(tab);
            const int view = studio2vid_tab_view(tab);
            const int wantw = view == STUDIO2VID_V_DISPLAY ? 100 : view == STUDIO2VID_V_BEAM ? 112 : 64;
            const int wanth = view == STUDIO2VID_V_DISPLAY ? 246 : view == STUDIO2VID_V_BEAM ? 262 : 128;
            if (wide != wantw * zoom || high != wanth * zoom) {
                check(0, "fuzz: the picture has the size of its view");
                break;
            }
            for (int probe = 0; probe < 6; probe++) {
                studio2vid_tab_hover(tab, (int)(rnd() % (unsigned)(wide + 4)) - 2, (int)(rnd() % (unsigned)(high + 4)) - 2);
                if (probe == 3) studio2vid_tab_pin(tab, (int)(rnd() % (unsigned)wide), (int)(rnd() % (unsigned)high));
            }
            studio2vid_tab_unpin(tab);
        } else if (strlen(studio2vid_tab_why(tab)) == 0) {
            check(0, "fuzz: no picture comes with a reason");
            break;
        }
        {
            char text[16384];
            for (int sec = 0; sec < STUDIO2VID_S_COUNT; sec++)
                studio2vid_tab_text(tab, sec, text, sizeof text);
        }
    }
    check(failures == 0, "fuzz: 300 random captures survived");

    studio2vid_tab_free(tab);
    printf(failures ? "FAILED\n" : "all ok\n");
    return failures ? 1 : 0;
}
