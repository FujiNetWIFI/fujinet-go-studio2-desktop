/*
 * controls -- the on-screen Controllers: one layout for every toolkit, its
 * hit test and the key legends. Drawn from the real console's top (see
 * controllers-port/DESIGN-studio2.md): a long, low case with a recessed well
 * in each wing holding a 3x3 keypad with the 0 centred under the 8 (A on the
 * left, B on the right), and between them the centre panel -- the gold plate
 * with the RCA badge and "Studio II", the ribbed panel with its silver plate,
 * the red LED (lit while CLEAR is held: the CPU is in reset) and the CLEAR
 * bar, and the two session buttons CONFIG and STOP (not on the real console,
 * drawn as two small console-style buttons in the ribbed area).
 *
 * Units: a keypad key's width is 1.0 (the keys are 1.25 : 1, wider than
 * tall: 1.25 x 1.0 here, so a "unit" is a key's height). The extents are the
 * controls' bounding box; a toolkit adds its own margin and scale. No colours
 * here. Entries are in paint order; only KEY, CLEAR and SYSBUTTON are
 * pressable (hit test: exact rectangles, topmost first).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stddef.h>

#include "studio2session.h"

/* ---- geometry (layout units) ------------------------------------------------ */

#define KEY_W        1.25f
#define KEY_H        1.0f
#define KEY_PITCH_X  1.6f
#define KEY_PITCH_Y  1.5f
#define WELL_W       5.6f
#define WELL_H       6.5f
#define WELL_Y       1.0f
#define WELL_A_X     0.8f
#define WELL_B_X     11.6f
#define WELL_PAD_X   ((WELL_W - (2 * KEY_PITCH_X + KEY_W)) / 2)
#define WELL_PAD_Y   0.5f
#define PANEL_X      7.0f
#define PANEL_W      4.0f
#define GOLD_H       1.8f
#define PLATE_X      8.0f
#define PLATE_Y      3.2f
#define PLATE_W      2.0f
#define PLATE_H      3.1f
#define LED_D        0.6f
#define CLEAR_W      1.6f
#define CLEAR_H      0.8f
#define SYS_W        1.7f
#define SYS_H        0.7f
#define SYS_Y        6.6f
#define LAYOUT_W     18.0f
#define LAYOUT_H     8.0f

#define TOTAL (2 /*wells*/ + 2 /*A B*/ + 3 /*plates*/ + 3 /*badge text*/ + 1 /*led*/ \
               + 2 * STUDIO2_ACT_PER_PORT + 1 /*clear*/ + 1 /*clear caption*/ + 2 /*sys*/)

static studio2_control g_layout[TOTAL];
static int g_count;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void add(int kind, const char *label, int port, int target,
                float xpos, float ypos, float width, float height)
{
    studio2_control *ctl = &g_layout[g_count++];
    ctl->kind = kind;
    ctl->label = label;
    ctl->port = port;
    ctl->target = target;
    ctl->xpos = xpos; ctl->ypos = ypos; ctl->width = width; ctl->height = height;
}

/* The key legends: the digit on a keypad key, and the console buttons. */
static const char *const k_digit[STUDIO2_ACT_PER_PORT] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
};

const char *studio2session_key_label(int target)
{
    if (target >= 0 && target < 2 * STUDIO2_ACT_PER_PORT)
        return k_digit[target % STUDIO2_ACT_PER_PORT];
    if (target == STUDIO2_TARGET_SWITCH(STUDIO2_SW_CLEAR)) return "CLEAR";
    if (target == STUDIO2_TARGET_SYSACT(STUDIO2_SYSACT_RESET_CONFIG)) return "CONFIG";
    if (target == STUDIO2_TARGET_SYSACT(STUDIO2_SYSACT_DEBUG_STOP)) return "STOP";
    return "";
}

/* Row and column of a keypad key: 1 2 3 / 4 5 6 / 7 8 9, then 0 under the 8. */
static void key_cell(int key, int *row, int *col)
{
    if (key == 0) { *row = 3; *col = 1; }
    else { *row = (key - 1) / 3; *col = (key - 1) % 3; }
}

static void build(void)
{
    static const char *const letters[2] = { "A", "B" };
    int port, key;

    g_count = 0;
    for (port = 0; port < 2; port++)
        add(STUDIO2_CTL_WELL, letters[port], port, -1,
            port ? WELL_B_X : WELL_A_X, WELL_Y, WELL_W, WELL_H);
    for (port = 0; port < 2; port++)
        add(STUDIO2_CTL_CAPTION, letters[port], port, -1,
            (port ? WELL_B_X : WELL_A_X) + WELL_W / 2 - 0.4f, 0.1f, 0.8f, 0.8f);

    add(STUDIO2_CTL_PANEL, "gold", -1, -1, PANEL_X, WELL_Y, PANEL_W, GOLD_H);
    add(STUDIO2_CTL_PANEL, "ribbed", -1, -1, PANEL_X, WELL_Y + GOLD_H,
        PANEL_W, WELL_H - GOLD_H);
    add(STUDIO2_CTL_PANEL, "silver", -1, -1, PLATE_X, PLATE_Y, PLATE_W, PLATE_H);
    add(STUDIO2_CTL_CAPTION, "RCA", -1, -1, PANEL_X + 0.2f, WELL_Y + 0.2f, 0.9f, 0.9f);
    add(STUDIO2_CTL_CAPTION, "Studio II", -1, -1, PANEL_X + 1.3f, WELL_Y + 0.2f, 2.5f, 0.8f);
    add(STUDIO2_CTL_CAPTION, "HOME TV PROGRAMMER", -1, -1,
        PANEL_X + 0.2f, WELL_Y + 1.2f, PANEL_W - 0.4f, 0.4f);

    add(STUDIO2_CTL_LED, "LED", -1, STUDIO2_TARGET_SWITCH(STUDIO2_SW_CLEAR),
        PLATE_X + (PLATE_W - LED_D) / 2, PLATE_Y + 0.4f, LED_D, LED_D);

    for (port = 0; port < 2; port++)
        for (key = 0; key < STUDIO2_ACT_PER_PORT; key++) {
            int row, col;
            key_cell(key, &row, &col);
            add(STUDIO2_CTL_KEY, k_digit[key], port, STUDIO2_TARGET_PORT(port, key),
                (port ? WELL_B_X : WELL_A_X) + WELL_PAD_X + (float)col * KEY_PITCH_X,
                WELL_Y + WELL_PAD_Y + (float)row * KEY_PITCH_Y, KEY_W, KEY_H);
        }

    add(STUDIO2_CTL_CLEAR, "CLEAR", -1, STUDIO2_TARGET_SWITCH(STUDIO2_SW_CLEAR),
        PLATE_X + (PLATE_W - CLEAR_W) / 2, PLATE_Y + 1.5f, CLEAR_W, CLEAR_H);
    add(STUDIO2_CTL_CAPTION, "CLEAR", -1, -1,
        PLATE_X + 0.1f, PLATE_Y + 1.5f + CLEAR_H + 0.1f, PLATE_W - 0.2f, 0.4f);

    add(STUDIO2_CTL_SYSBUTTON, "CONFIG", -1,
        STUDIO2_TARGET_SYSACT(STUDIO2_SYSACT_RESET_CONFIG),
        PANEL_X + 0.2f, SYS_Y, SYS_W, SYS_H);
    add(STUDIO2_CTL_SYSBUTTON, "STOP", -1,
        STUDIO2_TARGET_SYSACT(STUDIO2_SYSACT_DEBUG_STOP),
        PANEL_X + PANEL_W - 0.2f - SYS_W, SYS_Y, SYS_W, SYS_H);
}

static void ensure_built(void)
{
    pthread_once(&g_once, build);
}

const studio2_control *studio2session_controls_layout(int *count, float *width, float *height)
{
    ensure_built();
    if (count) *count = g_count;
    if (width) *width = LAYOUT_W;
    if (height) *height = LAYOUT_H;
    return g_layout;
}

int studio2session_controls_hit(float xpos, float ypos)
{
    int idx;

    ensure_built();
    for (idx = g_count - 1; idx >= 0; idx--) {
        const studio2_control *ctl = &g_layout[idx];
        if (ctl->kind != STUDIO2_CTL_KEY && ctl->kind != STUDIO2_CTL_CLEAR &&
            ctl->kind != STUDIO2_CTL_SYSBUTTON)
            continue;
        if (xpos >= ctl->xpos && xpos < ctl->xpos + ctl->width
            && ypos >= ctl->ypos && ypos < ctl->ypos + ctl->height)
            return idx;
    }
    return -1;
}
