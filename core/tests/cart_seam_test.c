/*
 * cart_seam_test -- the seam between the console and the FujiNet cartridge
 * (core/studio2/fujinet_cart.h), with the suite's own images and no FujiNet:
 *
 *   - routing: a game (no claim) is served as ROM at $0400-$07FF with no
 *     mailbox, hotspots or raster, so those pages fall through to the
 *     console; a FujiNet app (the claim at $07FC) gets the arena, the
 *     hotspot pages and the raster;
 *   - peeks fire nothing: the debugger's peek of a hotspot changes no
 *     state, and a peek of the raster serves 0 without moving the beam;
 *   - the hand-over: a read of $0000 swaps an armed image in only when it
 *     is qualified -- straight after the stub's FN_STUB_T, or after the bus
 *     went quiet (the console in CLEAR) -- never on any other read of
 *     $0000, and never on a peek;
 *   - CLEAR on the machine: an app that armed CONFIG and holds CLEAR comes
 *     back up in CONFIG.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "fuji_mailbox.h"
#include "s2map.h"
#include "s2_cart.h"
#include "studio2_internal.h"
#include "test_rom.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static studio2_cart_t *cart;
static uint32_t t_us = 1000;

/* a console read, the bus time moving on as the console's would */
static uint8_t rd(uint16_t a)
{
    uint8_t d = 0xFF;
    t_us += 9;
    studio2_cart_read(cart, a, t_us, &d);
    return d;
}

static uint8_t pk(uint16_t a)
{
    uint8_t d = 0xFF;
    studio2_cart_peek(cart, a, &d);
    return d;
}

static void power_on(const uint8_t *image, uint32_t len)
{
    char why[200];
    if (studio2_cart_power_on(cart, image, len, "127.0.0.1:1", true, why, sizeof why) != 0)
        printf("  power on: %s\n", why);
    t_us = 1000;
}

static studio2_cart_status_t status(void)
{
    studio2_cart_status_t st;
    studio2_cart_status(cart, &st);
    return st;
}

/* FN_REG_CONFIG = FN_CONFIG_MAGIC, by reads: stage and arm CONFIG */
static void arm_config(uint8_t (*read)(uint16_t))
{
    read((uint16_t)(FN_ARENA_BASE + FN_H_REGSEL + FN_REG_CONFIG));
    read((uint16_t)(FN_ARENA_BASE + FN_H_REGDATA + FN_CONFIG_MAGIC));
}

static studio2_machine_t m;

static void frames(int n)
{
    for (int i = 0; i < n; i++)
    {
        studio2_machine_latch_inputs(&m);
        studio2_machine_run_frame(&m);
        studio2_cart_frame(m.cart);
        m.out_frames = 0;
    }
}

int main(void)
{
    uint8_t types[256];
    studio2_cart_status_t st;
    uint8_t base;

    cart = studio2_cart_create();

    /* ---- a game: ROM in the window, nothing else ---- */
    power_on(test_counter_game, sizeof test_counter_game);
    st = status();
    check(!st.mailbox_mode && !st.live_mailbox && st.mode == FN_MODE_GAME,
          "a game is served alone: no mailbox, mode GAME");
    studio2_cart_page_types(cart, types);
    check(types[0x04] == S2PG_ROM && types[0x07] == S2PG_ROM, "its window, $0400-$07FF, is ROM");
    check(types[0x00] == S2PG_NONE && types[0x08] == S2PG_NONE && types[0xE4] == S2PG_NONE &&
          types[0xE9] == S2PG_NONE && types[0xF4] == S2PG_NONE,
          "the BIOS, RAM, arena, hotspot and raster pages are the console's");
    check(rd(0x0400) == test_counter_game[0] && rd(0x07FF) == test_counter_game[0x3FF],
          "the console reads the image");
    {
        uint8_t d = 0x5A;
        check(studio2_cart_read(cart, 0xE905, t_us += 9, &d) == 0 && d == 0x5A,
              "a read the cart does not own leaves the bus to the console");
    }

    /* ---- an app: the mailbox ---- */
    power_on(test_counter_app, sizeof test_counter_app);
    st = status();
    check(st.mailbox_mode && st.live_mailbox && st.direct, "a FujiNet app keeps the mailbox");
    studio2_cart_page_types(cart, types);
    check(types[0x04] == S2PG_ROM && types[0xE4] == S2PG_ROM && types[0xE8] == S2PG_ROM,
          "its window and the arena (reply, status page) are ROM");
    check(types[0xE9] == S2PG_HOT && types[0xEB] == S2PG_HOT, "REGSEL to DATA are hotspots");
    check(types[0xF4] == S2PG_RASTER && types[0xFF] == S2PG_RASTER, "$F400 up is the raster");
    check(pk(FN_CLAIM_ADDR) == 'F' && pk(FN_CLAIM_ADDR + 3) == 'I', "the claim is served");
    check(pk(FN_ARENA_BASE + FN_R_MAGIC0) == 'F' && pk(FN_ARENA_BASE + FN_R_MAGIC1) == 'N',
          "the status page carries the cart's magic");

    /* peeks fire nothing */
    arm_config(pk);
    st = status();
    check(!st.armed && !st.staged && st.queue == 0, "a peek of the hotspots arms nothing");
    {
        uint8_t frames_before = status().frames;
        check(pk(FN_RASTER_BASE) == 0 && pk(FN_RASTER_BASE + 7) == 0, "a peek of the raster serves 0");
        rd(0x0400);
        t_us += 20000;          /* a VBLANK's silence: the next DMA read starts a frame */
        rd(FN_RASTER_BASE);
        check((uint8_t)(status().frames - frames_before) == 1,
              "and moves no beam: the next real DMA read starts one frame");
    }

    /* the hand-over (DIRECT counts as the first swap: the image is served
     * as if the cart had planned and swapped it in) */
    base = status().swaps;
    arm_config(rd);
    st = status();
    check(st.armed && st.staged && st.staged_config, "the reads arm CONFIG");
    pk(FN_STUB);
    pk(FN_STUB_T);
    pk(0x0000);
    check(status().swaps == base, "the stub's reads peeked swap nothing");
    rd(FN_STUB_T);
    rd(0x0010);
    rd(0x0000);
    check(status().swaps == base, "a read of $0000 that does not follow FN_STUB_T swaps nothing");
    rd(FN_STUB);
    rd(FN_STUB + 1);
    rd(FN_STUB_T);
    rd(0x0000);
    st = status();
    check(st.swaps == base + 1 && !st.armed && !st.staged,
          "straight after FN_STUB_T it swaps CONFIG in");
    check(st.live_mailbox && st.live_size > sizeof test_counter_app && rd(FN_CLAIM_ADDR) == 'F',
          "and serves it");

    /* the quiet bus: what the console in CLEAR looks like to the cart */
    power_on(test_counter_app, sizeof test_counter_app);
    base = status().swaps;
    arm_config(rd);
    rd(0x0123);
    rd(0x0000);
    check(status().swaps == base, "a read of $0000 straight after another swaps nothing");
    t_us += S2_SILENCE_US + 1;
    rd(0x0000);
    check(status().swaps == base + 1, "after the bus went quiet a read of $0000 swaps");
    studio2_cart_power_off(cart);

    /* ---- CLEAR on the machine ---- */
    power_on(test_counter_app, sizeof test_counter_app);
    studio2_machine_init(&m, NULL, 0, cart, false);
    frames(20);
    {
        uint8_t a = studio2_machine_peek(&m, TEST_RAM_COUNTER);
        frames(5);
        check(studio2_machine_peek(&m, TEST_RAM_COUNTER) != a, "the app runs on the stand-in");
    }
    {
        /* the app's own reads, at the machine's bus time */
        uint32_t now = (uint32_t)(m.cycles * 1000000ULL / STUDIO2_CPU_CLOCK);
        uint8_t d;
        studio2_cart_read(cart, (uint16_t)(FN_ARENA_BASE + FN_H_REGSEL + FN_REG_CONFIG), now + 1, &d);
        studio2_cart_read(cart, (uint16_t)(FN_ARENA_BASE + FN_H_REGDATA + FN_CONFIG_MAGIC), now + 2, &d);
    }
    check(status().armed, "the app armed CONFIG");
    base = status().swaps;
    frames(10);
    check(status().swaps == base, "running on, nothing swaps (the app never runs the stub)");
    m.clear_live = true;
    frames(3);
    m.clear_live = false;
    frames(30);
    st = status();
    check(st.swaps == base + 1 && st.live_mailbox && st.live_size > sizeof test_counter_app,
          "CLEAR, released: the console comes back up in CONFIG");

    studio2_cart_destroy(cart);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
