/*
 * vidtab_live_test -- the Video tab against the real machine: the CONFIG
 * cartridge on the stand-in (real 1861 DMA from the cartridge's raster, no
 * ROMs). Running: the last finished frame, the capture's per-line records and
 * the tables. Stopped: Display is exactly the frame the session publishes,
 * Decoded (rebuilt from each line's DMA bytes) agrees with Display pixel for
 * pixel, and the machine runs on again afterwards.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "studio2debug.h"
#include "studio2session.h"
#include "studio2vidcap.h"
#include "studio2vidtab.h"
#include "host.h"
#include "test_tmpdir.h"

static int failures;
static void check(int good, const char *what)
{
    printf("%s: %s\n", good ? "ok" : "FAIL", what);
    if (!good) failures++;
}

static void sleep_ms(int millis)
{
    struct timespec nap = { millis / 1000, (long)(millis % 1000) * 1000000L };
    nanosleep(&nap, NULL);
}

static int wait_stopped(studio2debug *dbg, int want, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (!!studio2debug_is_stopped(dbg) == want) return 1;
        sleep_ms(5);
    }
    return 0;
}

static uint32_t frame_copy[STUDIO2SESSION_FB_WIDTH * STUDIO2SESSION_FB_HEIGHT];
static uint32_t display_copy[STUDIO2VID_VIS_W * STUDIO2VID_VIS_H];
static studio2vid_capture running_cap;

int main(void)
{
    char cfg[512], data[512];
    studio2session_paths paths;
    studio2session *sess;
    studio2session_start_opts opts;
    studio2debug *dbg;
    studio2vid_tab *tab;
    const uint32_t *pic;
    int wide, high;

    test_tmpdir(cfg, sizeof cfg, "vcfg");
    test_tmpdir(data, sizeof data, "vdata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg; paths.data_dir = data; paths.fujinet_lib = "";
    sess = studio2session_new(&paths);
    if (!sess) return 1;
    studio2session_default_opts(sess, &opts);
    opts.enable_fujinet = 0; opts.enable_audio = 0; opts.enable_gamepad = 0;
    check(studio2session_start(sess, &opts) == 0, "session starts (CONFIG)");
    dbg = studio2session_debugger(sess);
    tab = studio2vid_tab_new(dbg);
    check(tab != NULL, "tab");
    {
        const uint64_t start = studio2session_frame_count(sess);
        for (int waited = 0; waited < 8000 && studio2session_frame_count(sess) < start + 120; waited += 10)
            sleep_ms(10);
    }

    /* running */
    studio2vid_tab_set_zoom(tab, 1);
    studio2vid_tab_set_overlays(tab, 0);
    check((studio2vid_tab_refresh(tab, 1) & STUDIO2VID_TAB_PICTURE) != 0, "refresh while running");
    pic = studio2vid_tab_picture(tab, &wide, &high);
    check(pic && wide == 100 && high == 246, "Display (running) is 100 x 246");
    {
        uint64_t serial = 0;
        int full = 0, raster = 0, ink = 0, fresh = 0;
        check(studio2_host_capture_get(&running_cap, &serial) == 1 && serial != 0, "the host captured a finished frame");
        check(studio2_host_capture_get(&running_cap, &serial) == 0, "...and a second get with the same serial copies nothing");
        check(running_cap.beam_row == STUDIO2VID_RASTER_H && running_cap.disp, "a finished frame: all rows drawn, display on");
        for (int row = STUDIO2VID_DISP_Y0; row < STUDIO2VID_DISP_Y1; row++) {
            const studio2vid_line *rec = &running_cap.lines[row];
            full += rec->valid && rec->count == 8;
            raster += rec->valid && rec->src == STUDIO2VID_SRC_RASTER;
            fresh += rec->fresh;
        }
        for (int pix = 0; pix < (int)sizeof running_cap.bits; pix++) ink += running_cap.bits[pix];
        printf("  %d full bursts, %d from the cart raster, %d fresh, %d lit pixels, short lines %u\n",
               full, raster, fresh, ink, running_cap.short_lines);
        check(full == 128 && raster == 128 && fresh == 128, "every display line: 8 bytes from the cartridge's raster, this frame");
        check(ink > 200, "CONFIG has ink");
        check(running_cap.dma_bytes > 128u * 8u && running_cap.inp1_count > 0, "DMA and display-on strobes counted");
    }
    {
        const studio2vid_line_row *rows;
        const studio2vid_reg *regs;
        char text[4096];
        check(studio2vid_tab_lines(tab, &rows) >= 128, "the Lines table has the display lines");
        check(studio2vid_tab_regs(tab, &regs) >= 10, "registers");
        check(studio2vid_tab_text(tab, STUDIO2VID_S_TIMING, text, sizeof text) > 20 && strstr(text, "Short bursts"),
              "timing text");
    }
    studio2vid_tab_set_view(tab, STUDIO2VID_V_BEAM);
    studio2vid_tab_refresh(tab, 1);
    check(studio2vid_tab_picture(tab, &wide, &high) == NULL && strlen(studio2vid_tab_why(tab)) > 10,
          "Beam while running says why");
    studio2vid_tab_set_view(tab, STUDIO2VID_V_DISPLAY);

    /* stopped, several times over: a stop is only reported once its state is published */
    for (int round = 0; round < 4; round++) {
        uint64_t serial = 0;
        int diff = -1, mismatch = 0, compared = 0, tries;

        studio2debug_attach(dbg);
        check(wait_stopped(dbg, 1, 3000), "stopped");
        for (tries = 0; tries < 5 && diff != 0; tries++) {
            sleep_ms(30);
            studio2vid_tab_set_view(tab, STUDIO2VID_V_DISPLAY);
            studio2vid_tab_refresh(tab, 1);
            pic = studio2vid_tab_picture(tab, &wide, &high);
            serial = 0;
            studio2session_copy_frame(sess, frame_copy, &serial);
            diff = pic && wide == STUDIO2SESSION_FB_WIDTH && high == STUDIO2SESSION_FB_HEIGHT ? 0 : -1;
            for (int pix = 0; diff == 0 && pix < wide * high; pix++)
                diff += pic[pix] != (frame_copy[pix] | 0xff000000u);
        }
        check(diff == 0, "stopped: Display is exactly the session's frame copy");
        if (pic) memcpy(display_copy, pic, sizeof display_copy);

        /* Decoded against Display, through the lines table */
        {
            const studio2vid_line_row *rows;
            const int nrows = studio2vid_tab_lines(tab, &rows);
            const uint32_t tint_on = 0xffd0e4ffu;
            studio2vid_tab_set_view(tab, STUDIO2VID_V_DECODED);
            studio2vid_tab_refresh(tab, 1);
            pic = studio2vid_tab_picture(tab, &wide, &high);
            check(pic && wide == 64 && high == 128, "Decoded is 64 x 128");
            for (int idx = 0; pic && idx < nrows; idx++) {
                const studio2vid_line_row *row = &rows[idx];
                if (!row->display || row->count != 8 || row->hpos > 44) continue;
                for (int col = 0; col < 64; col++) {
                    const int rastx = row->hpos + 4 + col - STUDIO2VID_VIS_X0;
                    const int rasty = row->row - STUDIO2VID_VIS_Y0;
                    const int decoded_on = pic[(size_t)(row->row - STUDIO2VID_DISP_Y0) * 64 + (size_t)col] == tint_on;
                    const int shown_on = display_copy[(size_t)rasty * STUDIO2VID_VIS_W + (size_t)rastx] == 0xffffffffu;
                    compared++;
                    mismatch += decoded_on != shown_on;
                }
            }
            check(compared > 6000 && mismatch == 0, "Decoded (from the DMA bytes) matches Display pixel for pixel");
        }

        /* Beam works now, and the beam is where the CPU says */
        studio2vid_tab_set_view(tab, STUDIO2VID_V_BEAM);
        studio2vid_tab_refresh(tab, 1);
        pic = studio2vid_tab_picture(tab, &wide, &high);
        check(pic && wide == 112 && high == 262, "Beam is available while stopped");
        {
            studio2debug_cpu cpu;
            studio2debug_cpu_get(dbg, &cpu);
            check(studio2vid_tab_beam_permille(tab) >= 0 && cpu.vpos >= 0, "the beam is reported");
        }
        check(studio2vid_tab_break_on_pixel(tab) == -1, "Break on writes: none for the cartridge's raster");

        studio2debug_detach(dbg);
        check(wait_stopped(dbg, 0, 3000), "running again");
        sleep_ms(60);
    }

    /* Break on writes: a RAM-sourced pixel (a synthetic capture) gets a write
     * breakpoint on its byte's address, once; a raster pixel gets none */
    {
        static studio2vid_capture synth;
        studio2debug_breakpoint list[64];
        int count, found = 0, first, second;

        memset(&synth, 0, sizeof synth);
        synth.serial = 1;
        synth.disp = 1;
        synth.beam_row = STUDIO2VID_RASTER_H;
        for (int row = STUDIO2VID_DISP_Y0; row < STUDIO2VID_DISP_Y1; row++) {
            studio2vid_line *rec = &synth.lines[row];
            rec->valid = rec->fresh = 1;
            rec->count = 8;
            rec->hpos = 12;
            rec->src = row == 100 ? STUDIO2VID_SRC_RASTER : STUDIO2VID_SRC_RAM;
            rec->addr = (uint16_t)(0x0900 + (row - STUDIO2VID_DISP_Y0) / 4 * 8);
        }
        studio2vid_tab_feed(tab, &synth, 0);
        studio2vid_tab_set_view(tab, STUDIO2VID_V_DECODED);
        studio2vid_tab_set_zoom(tab, 1);
        studio2vid_tab_refresh(tab, 1);
        studio2vid_tab_hover(tab, 8 * 2 + 1, 40);              /* line 120, byte 2: $0950 + 2 */
        check(studio2vid_tab_can_break(tab), "a RAM pixel can break");
        first = studio2vid_tab_break_on_pixel(tab);
        second = studio2vid_tab_break_on_pixel(tab);
        check(first >= 0 && first == second, "Break on writes adds one breakpoint, not two");
        count = studio2debug_breakpoint_list(dbg, list, 64);
        for (int i = 0; i < count; i++)
            found |= list[i].id == first && (list[i].type & STUDIO2DEBUG_BP_WRITE) &&
                     list[i].start == 0x0952 && list[i].end == 0x0952;
        check(found, "...a write breakpoint on the byte's address ($0952)");
        studio2vid_tab_hover(tab, 8 * 2 + 1, 20);              /* line 100: the raster */
        check(!studio2vid_tab_can_break(tab) && studio2vid_tab_break_on_pixel(tab) == -1,
              "a cartridge-raster pixel cannot");
        studio2debug_breakpoint_clear(dbg);
    }

    studio2vid_tab_free(tab);
    studio2session_stop(sess);
    studio2session_free(sess);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
