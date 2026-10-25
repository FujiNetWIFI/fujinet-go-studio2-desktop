/* fujinet_cart.h -- the FujiNet Studio II cartridge, the one cart this core
 * has.
 *
 * A C transposition of the firmware's own MAME device model
 * (fujinet-firmware pico/studio2/emu/studio2_fujinet.cpp, BSD-3-Clause,
 * Thomas Cherryhomes): an RP2354B on the 22-pin edge that sees the full
 * address of every console read -- display DMA included -- and claims any
 * page outside the BIOS with CART CS. The bus decision (s2_cart.h), the
 * image planner and page view (s2map.c), staging and the swap (fuji_load.c),
 * the text engine (s2_text.c), the protocol (fujimail.c) and the wire codec
 * (fujibus.c) are the cartridge firmware's own sources, staged verbatim into
 * fuji-generated/ by cmake/StageFujiProto.cmake; this device is only the
 * port.
 *
 * The machine hands every read to studio2_cart_read() with the bus time in
 * µs; the cart drives it or leaves it to the console (the BIOS, console RAM
 * or open bus). The console has no write strobe on the edge, so writes never
 * reach the cart.
 *
 * Like the cartridge, the mailbox service and the text engine run on their
 * own thread (the RP2354B's core0) while the bus is served on the emulation
 * thread (core1), so a slow network transaction never stalls the picture; a
 * synchronous mode (FUJINET_SYNC=1, or the sync flag) runs them inline
 * instead, which is deterministic for tests.
 *
 * Single instance per process: fujimail's port interface is C function
 * pointers with no context argument (the constraint is real hardware's
 * too -- one cart slot, one cart).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STUDIO2_FUJINET_CART_H
#define STUDIO2_FUJINET_CART_H

#include <stdbool.h>
#include <stdint.h>

/* The largest image the cartridge plans (s2map's S2MAP_IMAGE_MAX: an ST2's
 * header and 192 blocks), as messages print it; fujinet_cart.c holds the two
 * to each other. */
#define STUDIO2_CART_MAX_TEXT "49,408"
#define STUDIO2_CART_MAX_BYTES 49408u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct studio2_cart studio2_cart_t;

/* Create the (single) cart; NULL if one already exists. */
studio2_cart_t *studio2_cart_create(void);
void studio2_cart_destroy(studio2_cart_t *c);

/* Power on, as MAME's device_reset with the environment it reads:
 *   - `image` NULL: the baked CONFIG client, mailbox live ($FUJINET=1);
 *   - an image: planned and swapped in at power-on, as $FUJINET_IMAGE does
 *     (DIRECT). A FujiNet app (the "FUJI" claim at $07FC) keeps the mailbox
 *     and goes back to the baked CONFIG through FN_REG_CONFIG; a game alone
 *     needs no FujiNet, so it gets no socket.
 * `boip` is fujinet-pc's BoIP listener ("host:port"), or "" for none at all
 * (FujiNet switched off: nothing is dialled, every transaction fails at
 * once). Returns 0, or -1 with the reason (an image the cartridge cannot
 * plan) in `why`. */
int studio2_cart_power_on(studio2_cart_t *c, const uint8_t *image, uint32_t len,
                          const char *boip, bool sync, char *why, int whysz);
/* Power off (PWR_OK lost): stop the mailbox worker, close the link. */
void studio2_cart_power_off(studio2_cart_t *c);

/* ---- the bus ----
 * One console read at `a`, at bus time `t_us`. Returns 1 with *data set
 * when the cart drives the read (CART CS), 0 when it leaves it to the
 * console. Every read counts -- the raster is served by DMA timing, and a
 * read of $0000 after the hand-over stub or a silence is the swap. */
int studio2_cart_read(studio2_cart_t *c, uint16_t a, uint32_t t_us, uint8_t *data);

/* What the cart would drive at `a`, with no side effects (the raster's pages
 * read 0, as MAME's device does with side effects disabled). Returns 1 when
 * the page is the cart's. */
int studio2_cart_peek(studio2_cart_t *c, uint16_t a, uint8_t *out);
/* The debugger's poke: the byte behind a claimed page (the image buffer or
 * the arena), without firing a hotspot. Returns 1 when the page is the
 * cart's and writable. */
int studio2_cart_poke(studio2_cart_t *c, uint16_t a, uint8_t d);

/* Bring what the mailbox worker published into the arena (also done on
 * every read); the host calls it once a frame. */
void studio2_cart_frame(studio2_cart_t *c);

/* ---- status, for the session and the debugger ---- */
typedef struct {
    bool powered;
    bool mailbox_mode;        /* a socket and fujimail (else a game, DIRECT) */
    bool link_up;             /* the BoIP socket is connected */
    bool busy;                /* a FujiBus transaction is in flight */
    bool direct;              /* an opened image, served from power-on */
    bool booted_game;         /* anything other than CONFIG is being served */
    int mode;                 /* FN_MODE_* as published */
    bool live_mailbox;        /* the image being served claims the mailbox */
    bool live_st2;
    uint8_t live_blocks;
    uint32_t live_size, live_crc;
    bool staged, staged_claim, staged_config, armed;
    uint32_t staged_crc;
    uint8_t swaps;
    uint8_t nbanks, bank;     /* the bank window: banks, the one shown */
    int boot_state, boot_pct, boot_err;
    uint32_t boot_got, boot_total;
    uint8_t ackseq, status, err, reply_cmd;
    uint16_t rxlen;
    uint8_t frames;           /* raster frames served, mod 256 */
    uint32_t short_lines;     /* DMA bursts that ended short of 8 */
    unsigned queue;           /* hotspot events waiting for the worker */
} studio2_cart_status_t;

void studio2_cart_status(studio2_cart_t *c, studio2_cart_status_t *out);

/* The page view the bus serves: S2PG_* per 256-byte page. */
void studio2_cart_page_types(studio2_cart_t *c, uint8_t out[256]);

/* The text engine as drawn (the debugger's Cart tab). */
typedef struct {
    uint8_t chars[10][16];
    uint8_t inv[10][16];
    uint8_t row, col;
    bool inverse;
} studio2_cart_text_t;
void studio2_cart_text(studio2_cart_t *c, studio2_cart_text_t *out);

/* Copy the arena (2K), the raster (1K) or the live image buffer for the
 * debugger's Save. Return bytes copied. */
uint32_t studio2_cart_copy_arena(studio2_cart_t *c, uint8_t *dst, uint32_t max);
uint32_t studio2_cart_copy_raster(studio2_cart_t *c, uint8_t *dst, uint32_t max);

/* 1 if `image` (len bytes) can run on the cartridge, with *claims set when
 * it is a FujiNet app; else 0 and why. */
int studio2_cart_check_image(const uint8_t *image, uint32_t len, bool *claims,
                             char *why, int whysz);

#ifdef __cplusplus
}
#endif

#endif /* STUDIO2_FUJINET_CART_H */
