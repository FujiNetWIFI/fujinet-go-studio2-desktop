/*
 * gamepad_map_test -- the gamepad layer end to end with an SDL3 virtual
 * gamepad: hot-plug pickup, the default mapping onto the keypad (South = 0,
 * East = 5, the D-pad and the left stick as 2 4 6 8 with the diagonals on
 * 1 3 7 9) and the console's CLEAR (Start), and hot-unplug releasing
 * everything the pad held -- with no hardware and no human.
 *
 * What the pad holds is read back through studio2session_buttons_held and
 * studio2session_switch_held, which report the same per-source bits the machine
 * is driven from. SKIPs (77) where SDL has no virtual joysticks or a
 * machine's real pads leave no port free.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "studio2session.h"
#include "test_tmpdir.h"

#define PAD_NAME "studio2test virtual pad"

static int failures;
static int vport;   /* the keypad the virtual pad was given */
static int other_port = -1;   /* the keypad the second pad was left assigned to */

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* The pad thread syncs devices a few times a second and polls state
 * faster; give it ten seconds (a slow CI runner). */
static void expect(studio2session *s, unsigned want, int clear, const char *what)
{
    unsigned held = 0;
    int c = 0;
    for (int i = 0; i < 500; i++) {   /* 10 s */
        held = studio2session_buttons_held(s, vport);
        c = studio2session_switch_held(s, STUDIO2_SW_CLEAR);
        if (held == want && c == clear) {
            printf("ok: %s\n", what);
            return;
        }
        sleep_ms(20);
    }
    printf("FAIL: %s (held %03X want %03X, CLEAR %d/%d)\n", what, held, want, c, clear);
    failures++;
}

#define B(a) (1u << (a))

static int pad_index(studio2session *s, const char *want)
{
    char name[64];
    for (int i = 0; i < studio2session_gamepad_count(s); i++) {
        studio2session_gamepad_name(s, i, name, sizeof name);
        if (strcmp(name, want) == 0)
            return i;
    }
    return -1;
}

/* Wait (10 s) for the named pad to be there (or gone). */
static int wait_pad(studio2session *s, const char *name, int want_present)
{
    for (int i = 0; i < 500; i++) {
        if ((pad_index(s, name) >= 0) == want_present)
            return 1;
        sleep_ms(20);
    }
    return 0;
}

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static SDL_JoystickID attach(const char *name, Uint16 product, SDL_Joystick **joy)
{
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID id;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.vendor_id = 0x1209;
    desc.product_id = product;
    desc.name = name;
    id = SDL_AttachVirtualJoystick(&desc);
    *joy = id ? SDL_OpenJoystick(id) : NULL;
    return id;
}

int main(void)
{
    char cfg[512], data[512];
    studio2session_paths p;
    studio2session *s;
    studio2session_start_opts o;
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID vid;
    SDL_Joystick *vj;
    int i, baseline;

    test_tmpdir(cfg, sizeof cfg, "gmcfg");
    test_tmpdir(data, sizeof data, "gmdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg;
    p.data_dir = data;
    p.fujinet_lib = "";      /* the pad only */
    s = studio2session_new(&p);
    if (!s)
        return 1;
    studio2session_default_opts(s, &o);
    o.enable_fujinet = 0;
    o.enable_audio = 0;
    o.enable_gamepad = 1;
    if (studio2session_start(s, &o) != 0) {
        printf("FAIL: start: %s\n", studio2session_last_error(s));
        return 1;
    }

    /* Real pads may already be attached: let the first device scan find
     * them, so the virtual one is known by the slot it adds. */
    sleep_ms(700);
    baseline = studio2session_gamepad_count(s);

    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = PAD_NAME;
    vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        printf("SKIP: no virtual joysticks here: %s\n", SDL_GetError());
        studio2session_free(s);
        return 77;
    }
    vj = SDL_OpenJoystick(vid);
    if (!vj)
        return 1;

    for (i = 0; i < 500 && studio2session_gamepad_count(s) <= baseline; i++)
        sleep_ms(20);
    vport = -1;
    for (i = 0; i < studio2session_gamepad_count(s); i++) {
        char name[64];
        studio2session_gamepad_name(s, i, name, sizeof name);
        if (strcmp(name, PAD_NAME) == 0)
            vport = studio2session_gamepad_effective_port(s, i);
    }
    if (vport < 0 || vport > 1) {
        printf("%s: the virtual pad got no keypad (%d; real pads attached?)\n",
               vport < 0 && studio2session_gamepad_count(s) <= baseline ? "FAIL" : "SKIP", vport);
        SDL_CloseJoystick(vj);
        SDL_DetachVirtualJoystick(vid);
        studio2session_free(s);
        return studio2session_gamepad_count(s) <= baseline ? 1 : 77;
    }
    printf("ok: the virtual pad was picked up as keypad %c\n", 'A' + vport);

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect(s, B(STUDIO2_ACT_0), 0, "South is key 0");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
    expect(s, 0, 0, "released");

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, true);
    expect(s, B(STUDIO2_ACT_5), 0, "East is key 5");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, false);
    expect(s, 0, 0, "released");

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    expect(s, B(STUDIO2_ACT_2), 0, "D-pad up is key 2");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    expect(s, B(STUDIO2_ACT_4), 0, "D-pad left is key 4");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    expect(s, B(STUDIO2_ACT_1), 0, "up and left together are key 1, not 2 and 4");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
    expect(s, 0, 0, "released");

    /* the stick past its threshold steers too (a pad that reports its D-pad
     * as axes would otherwise drive nothing) */
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, 20000);
    expect(s, B(STUDIO2_ACT_9), 0, "the stick right and down is key 9");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, 0);
    expect(s, 0, 0, "centred");

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, true);
    expect(s, 0, 1, "Start is CLEAR");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, false);
    expect(s, 0, 0, "released");

    /* A trigger past half travel used to deadlock the gamepad thread on its
     * own lock, and the Controllers panel's timers (same lock) froze the UI
     * with it. A hang here is a ctest timeout. */
    studio2session_binding_set_button(s, STUDIO2_TARGET_PORT(vport, STUDIO2_ACT_7),
                                      STUDIO2_PAD_BTN_LEFT_TRIGGER, NULL, 0);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    expect(s, B(STUDIO2_ACT_7), 0, "left trigger presses its binding");
    (void)studio2session_gamepad_generation(s);
    for (i = 0; i < studio2session_gamepad_count(s); i++)
        (void)studio2session_gamepad_effective_port(s, i);
    printf("ok: session lock still free with a trigger held\n");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    expect(s, 0, 0, "left trigger released");
    studio2session_gamepad_capture_begin(s);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    {
        int btn = STUDIO2_PAD_BTN_NONE, got = 0;
        for (i = 0; i < 500 && !got; i++) {
            got = studio2session_gamepad_capture_poll(s, &btn);
            if (!got) sleep_ms(20);
        }
        if (got && btn == STUDIO2_PAD_BTN_RIGHT_TRIGGER)
            printf("ok: map mode captures a trigger\n");
        else {
            printf("FAIL: map mode captures a trigger (%d)\n", btn);
            failures++;
        }
    }
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    studio2session_bindings_reset(s);

    /* sources are independent: the keyboard and the pad hold the same key,
     * and the pad's button and its D-pad (the stick source) too */
    {
        const int t5 = STUDIO2_TARGET_PORT(vport, STUDIO2_ACT_5);
        const uint32_t ksym = studio2session_binding_get(s, t5).keysym;
        studio2session_key(s, ksym, 1);
        expect(s, B(STUDIO2_ACT_5), 0, "the keyboard holds key 5");
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, true);
        sleep_ms(100);
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, false);
        sleep_ms(100);
        expect(s, B(STUDIO2_ACT_5), 0, "the pad letting go of 5 leaves the keyboard's hold");
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, true);
        sleep_ms(100);
        studio2session_release_all(s);
        expect(s, B(STUDIO2_ACT_5), 0, "the keyboard letting go of 5 leaves the pad's hold");
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, false);
        expect(s, 0, 0, "released");

        studio2session_binding_set_button(s, STUDIO2_TARGET_PORT(vport, STUDIO2_ACT_6),
                                          STUDIO2_PAD_BTN_WEST, NULL, 0);
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_WEST, true);
        expect(s, B(STUDIO2_ACT_6), 0, "the D-pad and a button both hold key 6");
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_WEST, false);
        sleep_ms(100);
        expect(s, B(STUDIO2_ACT_6), 0, "the button letting go leaves the D-pad's hold");
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
        expect(s, 0, 0, "released");
        studio2session_bindings_reset(s);
    }

    /* a second pad: both keypads at once, and unplugging one releases only
     * what it held */
    {
        SDL_Joystick *vj2;
        SDL_JoystickID vid2 = attach(PAD_NAME " 2", 0x0002, &vj2);
        int idx2, other = 1 - vport;
        if (!vid2 || !vj2 || !wait_pad(s, PAD_NAME " 2", 1)) {
            check(0, "a second virtual pad was picked up");
        } else {
            idx2 = pad_index(s, PAD_NAME " 2");
            /* a real pad may be on the first keypad: fix both ends by hand */
            studio2session_gamepad_assign(s, pad_index(s, PAD_NAME), vport);
            studio2session_gamepad_assign(s, idx2, other);
            check(studio2session_gamepad_effective_port(s, idx2) == other,
                  "the second pad plays the other keypad");
            sleep_ms(200);          /* the pad thread notices the move (and lets go) */
            SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
            SDL_SetJoystickVirtualButton(vj2, SDL_GAMEPAD_BUTTON_SOUTH, true);
            {
                int ok = 0;
                for (i = 0; i < 500 && !ok; i++) {
                    ok = studio2session_buttons_held(s, vport) == B(STUDIO2_ACT_0) &&
                         studio2session_buttons_held(s, other) == B(STUDIO2_ACT_0);
                    if (!ok) sleep_ms(20);
                }
                check(ok, "each pad holds 0 on its own keypad");
            }
            SDL_SetJoystickVirtualButton(vj2, SDL_GAMEPAD_BUTTON_SOUTH, false);
            sleep_ms(200);
            check(studio2session_buttons_held(s, vport) == B(STUDIO2_ACT_0) &&
                  studio2session_buttons_held(s, other) == 0, "one letting go leaves the other's hold");
            SDL_SetJoystickVirtualButton(vj2, SDL_GAMEPAD_BUTTON_SOUTH, true);
            sleep_ms(200);

            /* an explicit assignment is remembered by the settings, and a
             * reconnect gets it back */
            check(strstr(studio2session_get_str(s, "pad_assign", ""), PAD_NAME " 2") != NULL,
                  "an explicit assignment is written to the pad_assign setting");
            SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
            SDL_SetJoystickVirtualButton(vj2, SDL_GAMEPAD_BUTTON_SOUTH, false);
            SDL_CloseJoystick(vj2);
            SDL_DetachVirtualJoystick(vid2);
            check(wait_pad(s, PAD_NAME " 2", 0), "the second pad is unplugged");
            expect(s, 0, 0, "and nothing is left held");
            vid2 = attach(PAD_NAME " 2", 0x0002, &vj2);
            check(vid2 && vj2 && wait_pad(s, PAD_NAME " 2", 1), "and plugged back in");
            idx2 = pad_index(s, PAD_NAME " 2");
            check(idx2 >= 0 && studio2session_gamepad_assignment(s, idx2) == other,
                  "it gets its explicit assignment back");

            /* it stays assigned to the other keypad for the next run */
            SDL_CloseJoystick(vj2);
            SDL_DetachVirtualJoystick(vid2);
            wait_pad(s, PAD_NAME " 2", 0);
            other_port = other;
        }
    }

    /* hot-unplug while held: the keypad is released, not left holding the
     * last direction forever */
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect(s, B(STUDIO2_ACT_6) | B(STUDIO2_ACT_0), 0, "held before the unplug");
    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    for (i = 0; i < 500 && studio2session_gamepad_count(s) != baseline; i++)
        sleep_ms(20);
    if (studio2session_gamepad_count(s) == baseline)
        printf("ok: the unplugged pad is gone\n");
    else {
        printf("FAIL: the backend kept the detached pad (%d, baseline %d)\n",
               studio2session_gamepad_count(s), baseline);
        failures++;
    }
    expect(s, 0, 0, "and its keypad is released");

    /* ...and a new run restores it from the settings */
    if (other_port >= 0) {
        SDL_Joystick *vj2;
        SDL_JoystickID vid2;
        int idx2;
        studio2session_stop(s);
        studio2session_free(s);
        s = studio2session_new(&p);
        studio2session_default_opts(s, &o);
        o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 1;
        check(s && studio2session_start(s, &o) == 0, "a new session starts");
        vid2 = attach(PAD_NAME " 2", 0x0002, &vj2);
        check(vid2 && vj2 && wait_pad(s, PAD_NAME " 2", 1), "the second pad is plugged in");
        idx2 = pad_index(s, PAD_NAME " 2");
        check(idx2 >= 0 && studio2session_gamepad_assignment(s, idx2) == other_port,
              "and the next run restores its assignment from the settings");
        studio2session_gamepad_assign(s, idx2, -1);
        check(strstr(studio2session_get_str(s, "pad_assign", ""), PAD_NAME " 2") == NULL,
              "choosing Automatic forgets it");
        SDL_CloseJoystick(vj2);
        SDL_DetachVirtualJoystick(vid2);
        wait_pad(s, PAD_NAME " 2", 0);
    }

    studio2session_stop(s);
    studio2session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
