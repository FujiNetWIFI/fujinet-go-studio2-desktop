/*
 * controls_test -- the Controllers layout: all 23 targets are drawn exactly
 * once, the keypads are laid out as the real console's (1 2 3 / 4 5 6 /
 * 7 8 9, the 0 centred under the 8; A left of the centre panel, B right),
 * everything fits the extents, pressable controls do not overlap, and the
 * hit test agrees with the rectangles.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "studio2session.h"

static int failures;
static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

static int pressable(const studio2_control *c)
{
    return c->kind == STUDIO2_CTL_KEY || c->kind == STUDIO2_CTL_CLEAR ||
           c->kind == STUDIO2_CTL_SYSBUTTON;
}

int main(void)
{
    int count, i, j, seen[STUDIO2_TARGET_COUNT] = { 0 }, all = 1, fit = 1, clash = 0, hits = 1;
    float width, height;
    const studio2_control *lay = studio2session_controls_layout(&count, &width, &height);
    const studio2_control *key[2][STUDIO2_ACT_PER_PORT] = { { 0 } };

    check(lay && count > 23 && width > 0 && height > 0, "a layout with extents");
    check(STUDIO2_TARGET_COUNT == 23, "23 targets in all");

    for (i = 0; i < count; i++)
        if (pressable(&lay[i])) {
            if (lay[i].target < 0 || lay[i].target >= STUDIO2_TARGET_COUNT) all = 0;
            else seen[lay[i].target]++;
            if (lay[i].kind == STUDIO2_CTL_KEY)
                key[lay[i].port][lay[i].target % STUDIO2_ACT_PER_PORT] = &lay[i];
        }
    for (i = 0; i < STUDIO2_TARGET_COUNT; i++)
        if (seen[i] != 1) { all = 0; printf("  target %d drawn %d times\n", i, seen[i]); }
    check(all, "every one of the 23 targets is one pressable control");

    for (i = 0; i < count; i++)
        if (lay[i].xpos < 0 || lay[i].ypos < 0 || lay[i].width <= 0 || lay[i].height <= 0 ||
            lay[i].xpos + lay[i].width > width || lay[i].ypos + lay[i].height > height)
            fit = 0;
    check(fit, "everything fits the extents");

    for (i = 0; i < count; i++)
        for (j = i + 1; j < count; j++)
            if (pressable(&lay[i]) && pressable(&lay[j]) &&
                lay[i].xpos < lay[j].xpos + lay[j].width && lay[j].xpos < lay[i].xpos + lay[i].width &&
                lay[i].ypos < lay[j].ypos + lay[j].height && lay[j].ypos < lay[i].ypos + lay[i].height)
                clash++;
    check(clash == 0, "no two pressable controls overlap");

    /* the keypads as the console has them */
    for (int port = 0; port < 2; port++) {
        int ok = 1;
        for (i = 1; i <= 9; i++) {
            const studio2_control *k = key[port][i];
            const studio2_control *first = key[port][1];
            if (!k || !first) { ok = 0; break; }
            if (fabsf(k->xpos - (first->xpos + (float)((i - 1) % 3) * (key[port][2]->xpos - first->xpos))) > 1e-3f ||
                fabsf(k->ypos - (first->ypos + (float)((i - 1) / 3) * (key[port][4]->ypos - first->ypos))) > 1e-3f)
                ok = 0;
        }
        check(ok, port ? "keypad B is 1 2 3 / 4 5 6 / 7 8 9 on a grid" : "keypad A is 1 2 3 / 4 5 6 / 7 8 9 on a grid");
        check(key[port][0] && key[port][8] &&
              fabsf((key[port][0]->xpos + key[port][0]->width / 2) -
                    (key[port][8]->xpos + key[port][8]->width / 2)) < 1e-3f &&
              key[port][0]->ypos > key[port][8]->ypos,
              "and the 0 is centred under the 8");
    }
    check(key[0][9] && key[1][1] && key[0][9]->xpos < key[1][1]->xpos, "A is left of B");

    {
        const studio2_control *clr = NULL, *cfg = NULL, *stop = NULL, *led = NULL;
        for (i = 0; i < count; i++) {
            if (lay[i].kind == STUDIO2_CTL_CLEAR) clr = &lay[i];
            if (lay[i].kind == STUDIO2_CTL_LED) led = &lay[i];
            if (lay[i].target == STUDIO2_TARGET_SYSACT(STUDIO2_SYSACT_RESET_CONFIG)) cfg = &lay[i];
            if (lay[i].target == STUDIO2_TARGET_SYSACT(STUDIO2_SYSACT_DEBUG_STOP)) stop = &lay[i];
        }
        check(clr && led && cfg && stop, "CLEAR, its LED, CONFIG and STOP are drawn");
        check(clr && key[0][3] && key[1][1] && clr->xpos > key[0][3]->xpos + key[0][3]->width &&
              clr->xpos + clr->width < key[1][1]->xpos, "CLEAR sits between the keypads");
        check(clr && led && led->ypos < clr->ypos, "with the LED above it");
        check(clr && clr->target == STUDIO2_TARGET_SWITCH(STUDIO2_SW_CLEAR) && led->target == clr->target,
              "CLEAR's bar and LED are the CLEAR target");
    }

    /* the hit test: the middle of every pressable control finds it, a point
     * outside every one finds nothing */
    for (i = 0; i < count; i++) {
        int h;
        if (!pressable(&lay[i])) continue;
        h = studio2session_controls_hit(lay[i].xpos + lay[i].width / 2, lay[i].ypos + lay[i].height / 2);
        if (h != i) { hits = 0; printf("  control %d (%s) hit %d\n", i, lay[i].label, h); }
        if (studio2session_controls_hit(lay[i].xpos - 0.001f, lay[i].ypos - 0.001f) == i) hits = 0;
        if (studio2session_controls_hit(lay[i].xpos + lay[i].width + 0.001f, lay[i].ypos + lay[i].height / 2) == i) hits = 0;
    }
    check(hits, "the hit test finds each control's middle and respects its edges");
    check(studio2session_controls_hit(-1, -1) == -1 && studio2session_controls_hit(width + 1, height + 1) == -1 &&
          studio2session_controls_hit(0.1f, 0.1f) == -1, "and nothing outside them");
    {
        /* a drawn-only part is never hit: the middle of the keypad well between keys */
        int well = -1;
        for (i = 0; i < count; i++) if (lay[i].kind == STUDIO2_CTL_WELL) { well = i; break; }
        check(well >= 0 && studio2session_controls_hit(lay[well].xpos + 0.05f, lay[well].ypos + 0.05f) == -1,
              "the well itself is not pressable");
    }

    check(!strcmp(studio2session_key_label(STUDIO2_TARGET_PORT(1, STUDIO2_ACT_7)), "7"), "a key's legend is its digit");
    check(!strcmp(studio2session_key_label(STUDIO2_TARGET_PORT(0, STUDIO2_ACT_0)), "0"), "the 0's legend");
    check(!strcmp(studio2session_key_label(STUDIO2_TARGET_SWITCH(STUDIO2_SW_CLEAR)), "CLEAR"), "CLEAR's legend");
    check(!strcmp(studio2session_key_label(STUDIO2_TARGET_SYSACT(STUDIO2_SYSACT_RESET_CONFIG)), "CONFIG"), "CONFIG's legend");
    check(!strcmp(studio2session_key_label(STUDIO2_TARGET_SYSACT(STUDIO2_SYSACT_DEBUG_STOP)), "STOP"), "STOP's legend");
    check(!strcmp(studio2session_key_label(-1), "") && !strcmp(studio2session_key_label(99), ""), "no legend for no target");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
