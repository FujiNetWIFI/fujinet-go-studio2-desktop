/* fujinet_cart.c -- the FujiNet Studio II cartridge device (see
 * fujinet_cart.h).
 *
 * A C transposition of fujinet-firmware's own MAME cart model
 * (pico/studio2/emu/studio2_fujinet.cpp, BSD-3-Clause, copyright-holders
 * Thomas Cherryhomes), kept structurally line-for-line where C allows so the
 * two stay diffable: std::vector buffers become fixed allocations, the
 * std::thread worker a pthread, machine().side_effects_disabled() the peek
 * entry point, and the environment MAME's device_reset reads becomes
 * studio2_cart_power_on()'s arguments.
 *
 * Two things are this app's own, both modelled on the cartridge rather than
 * on MAME (and both as the SMS sibling's cart has them):
 *
 *   - FN_R_LINK is published after every service pass, as the firmware's
 *     fujinet.c does (the MAME device leaves it alone), and FN_R_STATUS
 *     shows a transaction in flight.
 *   - wait_link_ms reconnects: the console reaches its first transaction
 *     before FujiNet may be listening, and FujiNet can be restarted from its
 *     web UI under a running console. The MAME device never reconnects.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 * (the transposed device logic: BSD-3-Clause, see COMPLIANCE.md)
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fujinet_cart.h"

/* The cartridge firmware's own sources, staged verbatim by
 * cmake/StageFujiProto.cmake (see that file and COMPLIANCE.md). */
#include "fuji_mailbox.h"
#include "fujimail.h"
#include "fujitcp.h"
#include "fujitcp_host.h"
#include "fujiconfigrom.h"
#include "s2map.h"
#include "s2_cart.h"
#include "s2_text.h"
#include "fuji_load.h"

#define QUEUE_SIZE 4096
#define POKE_MAX   4096

/* the event queue's marker for a swap the CPU thread made */
#define EV_SWAPPED S2_RING_SWAP

typedef struct {
    uint16_t offset;
    uint8_t value;
} poke_t;

struct studio2_cart {
    uint8_t *buf[2];                    /* S2MAP_BUF_MAX each */
    uint8_t arena[FN_ARENA_SIZE];
    s2_text_t text;
    fuji_load_t load;
    s2_view_t *view;
    s2_bus_t bus;

    uint8_t *push;
    uint32_t push_len;
    uint8_t own_sel;

    uint32_t short_lines;               /* DMA bursts < 8 */
    uint16_t rline, rcol;               /* the last raster read's position */

    bool powered;
    bool mailbox_mode;                  /* a socket and fujimail, or DIRECT */
    bool direct;                        /* an opened image, served from power-on */
    bool debug;
    char boip[128];
    bool have_boip;

    /* The worker owns fujimail, the socket, the text engine and staging;
     * the CPU thread owns the arena and the view. Pokes reach the arena
     * through the poke list. */
    bool async;
    bool worker_running;
    pthread_t worker;
    pthread_mutex_t qlock;              /* queue, stop */
    pthread_cond_t qcv;
    uint16_t queue[QUEUE_SIZE];
    unsigned qhead, qtail;
    atomic_bool stop;
    pthread_mutex_t pklock;             /* pokes */
    poke_t pokes[POKE_MAX];
    unsigned npokes;
    atomic_bool poked;
    pthread_mutex_t loadlock;           /* load: staging against the swap */
    atomic_bool busy;
};

/* fujimail's port is C function pointers with no context argument; one
 * slot, one cart, as on the hardware. */
static studio2_cart_t *s_cart = NULL;

/* set on the worker thread: its pokes queue for the CPU thread */
static _Thread_local bool t_on_worker = false;

static void poke(studio2_cart_t *c, unsigned offset, uint8_t value);
static void publish_staged(studio2_cart_t *c);
static void hotspot(studio2_cart_t *c, uint16_t a);

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/*-------------------------------------------------
    C port callbacks
-------------------------------------------------*/

static void c_poke(unsigned offset, uint8_t value) { poke(s_cart, offset, value); }
static bool c_link_up(void) { return fujitcp_active(); }

static void publish_link(studio2_cart_t *c)
{
    poke(c, FN_R_LINK, fujitcp_active() ? 1 : 0);
}

/* FN_R_STATUS_BUSY is declared by the protocol but never raised by the
 * firmware, whose transaction is synchronous with its service loop. Here it
 * is real: the console keeps running while this blocks. fujimail
 * republishes FN_R_STATUS itself once it has the reply. */
static fb_status_t c_transact(uint8_t device, uint8_t command,
                              const fb_param_t *params, unsigned nparams,
                              const uint8_t *payload, uint16_t payload_len,
                              uint32_t timeout_ms, fb_reply_t *reply)
{
    studio2_cart_t *c = s_cart;
    fb_status_t st;

    atomic_store(&c->busy, true);
    poke(c, FN_R_STATUS, FN_R_STATUS_LINK | FN_R_STATUS_BUSY);
    st = fujitcp_transact(device, command, params, nparams, payload, payload_len,
                          timeout_ms, reply);
    if (st == FB_ENOLINK)
        fujitcp_close();   /* the next transaction reconnects */
    atomic_store(&c->busy, false);
    return st;
}

/* Try to connect for up to `ms` (see the header comment). */
static void c_wait_link(uint32_t ms)
{
    studio2_cart_t *c = s_cart;
    const uint64_t deadline = now_ms() + ms;

    while (!fujitcp_active() && !atomic_load(&c->stop))
    {
        if (fujitcp_init(c->have_boip ? c->boip : NULL) == 0)
            break;
        if (now_ms() >= deadline)
            break;
        sleep_ms(250);
    }
    publish_link(c);
}

static uint8_t c_stream_open(int stream, uint32_t size);
static void c_stream_write(int stream, const uint8_t *chunk, unsigned len);
static uint8_t c_stream_close(int stream, uint32_t got, bool aborted);
static void c_arm_swap(void);

static void c_on_txn(const fujimail_txn_t *t)
{
    char txt[40];
    unsigned k, m = 0;

    for (k = 0; k < t->rxlen && m < sizeof txt - 1; k++)
    {
        uint8_t ch = t->rx[k];
        if (ch == 0)
            break;
        txt[m++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
    }
    txt[m] = '\0';
    fprintf(stderr,
            "fujinet: dev=%02X cmd=%02X nparam=%u txlen=%u seq=%u"
            " -> err=%d reply=%02X rxlen=%u%s%s%s\n",
            t->device, t->command, t->nparam, t->txlen, t->seq,
            t->status, t->reply_cmd, t->rxlen,
            m ? " \"" : "", txt, m ? "\"" : "");
}

static void c_on_dbc(fujimail_dbc_ev_t ev, int stream, uint32_t expect, unsigned got, bool aborted)
{
    if (ev == FUJIMAIL_DBC_OPEN)
        fprintf(stderr, "fujinet: DBC open stream=%d size=%u\n", stream, expect);
    else
        fprintf(stderr, "fujinet: DBC close stream=%d got=%u%s\n",
                stream, got, aborted ? " ABORTED" : "");
}

static const fujimail_port_t debug_port = {
    c_poke, c_link_up, c_transact, fujitcp_send_bare,
    c_stream_open, c_stream_write, c_stream_close, c_arm_swap,
    c_wait_link, NULL, c_on_txn, c_on_dbc,
};

static const fujimail_port_t quiet_port = {
    c_poke, c_link_up, c_transact, fujitcp_send_bare,
    c_stream_open, c_stream_write, c_stream_close, c_arm_swap,
    c_wait_link, NULL, NULL, NULL,
};

/*-------------------------------------------------
    the arena
-------------------------------------------------*/

static void drain_pokes(studio2_cart_t *c)
{
    if (!atomic_load_explicit(&c->poked, memory_order_acquire))
        return;
    pthread_mutex_lock(&c->pklock);
    for (unsigned i = 0; i < c->npokes; i++)
        c->arena[c->pokes[i].offset] = c->pokes[i].value;
    c->npokes = 0;
    atomic_store_explicit(&c->poked, false, memory_order_relaxed);
    pthread_mutex_unlock(&c->pklock);
}

static void poke(studio2_cart_t *c, unsigned offset, uint8_t value)
{
    s2_text_boot_poke(&c->text, offset, value);
    if (offset >= FN_R_PAINT_END)
        return;
    if (!t_on_worker)
    {
        /* the worker may have queued older values: those go first */
        drain_pokes(c);
        c->arena[offset] = value;
        return;
    }
    pthread_mutex_lock(&c->pklock);
    if (c->npokes == POKE_MAX)
    {
        /* the CPU thread has not read the arena for a while (stopped in
         * the debugger): fold the list rather than lose the newest value */
        for (unsigned i = 0; i < c->npokes; i++)
            c->arena[c->pokes[i].offset] = c->pokes[i].value;
        c->npokes = 0;
    }
    c->pokes[c->npokes].offset = (uint16_t)offset;
    c->pokes[c->npokes].value = value;
    c->npokes++;
    pthread_mutex_unlock(&c->pklock);
    atomic_store_explicit(&c->poked, true, memory_order_release);
}

/*-------------------------------------------------
    hotspots and the swap
-------------------------------------------------*/

/* One hotspot read by console address: the text engine's, or the mailbox's
 * (with the cart's own FN_REG_CONFIG beside fujimail's registers). */
static void dispatch(studio2_cart_t *c, uint16_t a)
{
    if (a >= FN_TEXT_BASE)
    {
        s2_text_event(&c->text, a);
        return;
    }
    uint16_t off = (uint16_t)(a - FN_ARENA_BASE);
    unsigned page = off & FN_H_PAGE_MASK;
    if (page == FN_H_REGSEL)
        c->own_sel = (off & 0xFF) < 0x80 ? (uint8_t)(off & 0xFF) : c->own_sel;
    else if (page == FN_H_REGDATA && c->own_sel != 0xFF)
    {
        if (c->own_sel == FN_REG_CONFIG && (off & 0xFF) == FN_CONFIG_MAGIC)
        {
            pthread_mutex_lock(&c->loadlock);
            fuji_load_stage_config(&c->load, _configrom, FUJI_CONFIGROM_SIZE);
            fuji_load_arm(&c->load);
            pthread_mutex_unlock(&c->loadlock);
            publish_staged(c);
        }
        c->own_sel = 0xFF;
    }
    fujimail_read_hotspot(off);
}

/* The new image is running. If it claims the mailbox it finds the
 * interlock starting over and a blank screen, exactly as CONFIG does at
 * power-on. */
static void swapped(studio2_cart_t *c)
{
    uint8_t swaps;
    bool mailbox;

    pthread_mutex_lock(&c->loadlock);
    swaps = c->load.swaps;
    mailbox = c->load.view[c->load.live].mailbox;
    pthread_mutex_unlock(&c->loadlock);
    c->own_sel = 0xFF;
    if (mailbox)
    {
        s2_text_init(&c->text, c->arena + FN_R_DATA);
        fujimail_paint();
    }
    publish_staged(c);
    poke(c, FN_R_SWAPS, swaps);
    poke(c, FN_R_MODE, mailbox ? FN_MODE_APP : FN_MODE_GAME);
}

static void service(studio2_cart_t *c, uint16_t a)
{
    if (a != EV_SWAPPED)
        dispatch(c, a);
    else
        swapped(c);
}

/*-------------------------------------------------
    the mailbox worker (the cart's core0)
-------------------------------------------------*/

static unsigned queue_depth(studio2_cart_t *c)
{
    return (c->qhead + QUEUE_SIZE - c->qtail) % QUEUE_SIZE;
}

static void *worker_main(void *arg)
{
    studio2_cart_t *c = arg;

    t_on_worker = true;
    for (;;)
    {
        uint16_t a;

        pthread_mutex_lock(&c->qlock);
        if (queue_depth(c) == 0 && !atomic_load(&c->stop))
        {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 50 * 1000000L;
            if (until.tv_nsec >= 1000000000L)
            {
                until.tv_sec++;
                until.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&c->qcv, &c->qlock, &until);
        }
        if (atomic_load(&c->stop))
        {
            pthread_mutex_unlock(&c->qlock);
            break;
        }
        if (queue_depth(c) == 0)
        {
            pthread_mutex_unlock(&c->qlock);
            s2_text_tick(&c->text, (uint32_t)now_ms());
            continue;
        }
        a = c->queue[c->qtail];
        c->qtail = (c->qtail + 1) % QUEUE_SIZE;
        pthread_mutex_unlock(&c->qlock);

        service(c, a);
        publish_link(c);
    }
    return NULL;
}

static void worker_start(studio2_cart_t *c)
{
    atomic_store(&c->stop, false);
    c->qhead = c->qtail = 0;
    c->worker_running = false;
    if (!c->async)
        return;
    if (pthread_create(&c->worker, NULL, worker_main, c) == 0)
        c->worker_running = true;
    else
    {
        fprintf(stderr, "fujinet: no mailbox worker; servicing inline\n");
        c->async = false;
    }
}

/* A transaction in flight is cut short, so a power-off or exit never waits
 * out its timeout. */
static void worker_stop(studio2_cart_t *c)
{
    if (!c->worker_running)
        return;
    pthread_mutex_lock(&c->qlock);
    atomic_store(&c->stop, true);
    pthread_cond_broadcast(&c->qcv);
    pthread_mutex_unlock(&c->qlock);
    fujitcp_abort();
    pthread_join(c->worker, NULL);
    c->worker_running = false;
    c->qhead = c->qtail = 0;
    pthread_mutex_lock(&c->pklock);
    c->npokes = 0;
    atomic_store(&c->poked, false);
    pthread_mutex_unlock(&c->pklock);
}

/* A hotspot event (or a swap): to the worker, or served here when
 * synchronous. Nothing is dropped unless the ring is full, as on the
 * hardware. */
static void hotspot(studio2_cart_t *c, uint16_t a)
{
    if (!c->mailbox_mode)
        return;
    if (!c->worker_running)
    {
        service(c, a);
        publish_link(c);
        return;
    }
    pthread_mutex_lock(&c->qlock);
    unsigned next = (c->qhead + 1) % QUEUE_SIZE;
    if (next != c->qtail)
    {
        c->queue[c->qhead] = a;
        c->qhead = next;
    }
    pthread_cond_signal(&c->qcv);
    pthread_mutex_unlock(&c->qlock);
}

/*-------------------------------------------------
    the bus
-------------------------------------------------*/

int studio2_cart_read(studio2_cart_t *c, uint16_t a, uint32_t t, uint8_t *data)
{
    uint8_t d = 0xFF;
    unsigned r;

    if (!c->powered)
        return 0;
    drain_pokes(c);

    r = s2_bus_read(&c->bus, c->view, a, t, &d);
    if ((r & S2_DRIVE) && c->view->type[a >> 8] == S2PG_RASTER)
    {
        /* a burst that ended short of 8 reads: a line the 1861 drew wrong */
        if (c->bus.r_col == 0 && c->bus.r_line == c->rline + 1 && c->rcol != S2_LINE_BYTES - 1)
            c->short_lines++;
        c->rline = c->bus.r_line;
        c->rcol = c->bus.r_col;
    }
    if (r & S2_HOT_EV)
        hotspot(c, a);
    if (r & S2_RESET)
    {
        s2_view_t *nv;
        pthread_mutex_lock(&c->loadlock);
        nv = fuji_load_swap(&c->load);
        pthread_mutex_unlock(&c->loadlock);
        if (nv != c->view)
        {
            c->view = nv;
            if (c->debug)
                fprintf(stderr, "fujinet: swap %u -> %s\n", c->load.swaps,
                        nv->mailbox ? "app" : "game");
            hotspot(c, EV_SWAPPED);
        }
    }
    if (!c->worker_running && c->mailbox_mode && (t & 0x3FFF) < 8)
        s2_text_tick(&c->text, t / 1000);

    if (!(r & S2_DRIVE))
        return 0;
    *data = d;
    return 1;
}

int studio2_cart_peek(studio2_cart_t *c, uint16_t a, uint8_t *out)
{
    if (!c || !c->powered)
        return 0;
    drain_pokes(c);
    uint8_t ty = c->view->type[a >> 8];
    if (ty == S2PG_NONE)
        return 0;
    *out = ty == S2PG_RASTER ? 0 : c->view->page[a >> 8][a & 0xFF];
    return 1;
}

int studio2_cart_poke(studio2_cart_t *c, uint16_t a, uint8_t d)
{
    if (!c || !c->powered)
        return 0;
    uint8_t ty = c->view->type[a >> 8];
    if (ty != S2PG_ROM && ty != S2PG_HOT)
        return 0;
    const uint8_t *p = c->view->page[a >> 8] + (a & 0xFF);
    /* the served pages are this device's own buffers (or the baked zero
     * page, which is not writable) */
    for (int i = 0; i < 2; i++)
        if (p >= c->buf[i] && p < c->buf[i] + S2MAP_BUF_MAX)
        {
            c->buf[i][p - c->buf[i]] = d;
            return 1;
        }
    if (p >= c->arena && p < c->arena + FN_ARENA_SIZE)
    {
        drain_pokes(c);
        c->arena[p - c->arena] = d;
        return 1;
    }
    return 0;
}

void studio2_cart_frame(studio2_cart_t *c)
{
    if (c && c->powered)
        drain_pokes(c);
}

/*-------------------------------------------------
    push streams
-------------------------------------------------*/

static void publish_staged(studio2_cart_t *c)
{
    s2map_plan_t p;
    bool staged, staged_config, armed;
    uint8_t v = 0;

    pthread_mutex_lock(&c->loadlock);
    p = c->load.plan[c->load.live ^ 1u];
    staged = c->load.staged;
    staged_config = c->load.staged_config;
    armed = c->load.armed;
    pthread_mutex_unlock(&c->loadlock);

    if (staged)
    {
        v = FN_STAGED_READY;
        if (p.claim)
            v |= FN_STAGED_CLAIM;
        if (staged_config)
            v |= FN_STAGED_CONFIG;
    }
    for (unsigned i = 0; i < 4; i++)
        poke(c, FN_R_STAGED_CRC + i, (uint8_t)(p.crc >> (8 * i)));
    poke(c, FN_R_STAGED, v);
    poke(c, FN_R_ARMED, armed ? 1 : 0);
}

static uint8_t c_stream_open(int stream, uint32_t size)
{
    studio2_cart_t *c = s_cart;

    if (stream != FN_STREAM_ROM)
        return 0;
    uint8_t err = s2map_gate(size);
    if (err)
        return err;
    pthread_mutex_lock(&c->loadlock);
    c->push = fuji_load_target(&c->load);
    pthread_mutex_unlock(&c->loadlock);
    c->push_len = 0;
    publish_staged(c);
    return 0;
}

static void c_stream_write(int stream, const uint8_t *chunk, unsigned len)
{
    studio2_cart_t *c = s_cart;

    if (stream != FN_STREAM_ROM || !c->push)
        return;
    if (c->push_len + len > S2MAP_BUF_MAX)
        len = S2MAP_BUF_MAX - c->push_len;
    memcpy(c->push + c->push_len, chunk, len);
    c->push_len += len;
}

static uint8_t c_stream_close(int stream, uint32_t got, bool aborted)
{
    studio2_cart_t *c = s_cart;
    uint8_t err;
    s2map_plan_t p;

    (void)got;
    if (stream != FN_STREAM_ROM)
        return 0;
    if (aborted || !c->push || c->push_len == 0)
    {
        c->push = NULL;
        return 0;
    }
    pthread_mutex_lock(&c->loadlock);
    err = fuji_load_commit(&c->load, c->push_len);
    p = c->load.plan[c->load.live ^ 1u];
    pthread_mutex_unlock(&c->loadlock);
    c->push = NULL;
    publish_staged(c);
    if (!err && c->debug)
        fprintf(stderr, "fujinet: staged %u bytes, %s, %u blocks%s\n", p.size,
                p.st2 ? "ST2" : "raw", p.blocks, p.claim ? ", claimed" : "");
    return err;
}

static void c_arm_swap(void)
{
    studio2_cart_t *c = s_cart;

    pthread_mutex_lock(&c->loadlock);
    fuji_load_arm(&c->load);
    pthread_mutex_unlock(&c->loadlock);
    publish_staged(c);
}

/*-------------------------------------------------
    lifecycle
-------------------------------------------------*/

studio2_cart_t *studio2_cart_create(void)
{
    studio2_cart_t *c;

    if (s_cart)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->buf[0] = calloc(1, S2MAP_BUF_MAX);
    c->buf[1] = calloc(1, S2MAP_BUF_MAX);
    if (!c->buf[0] || !c->buf[1])
    {
        free(c->buf[0]);
        free(c->buf[1]);
        free(c);
        return NULL;
    }
    pthread_mutex_init(&c->qlock, NULL);
    pthread_cond_init(&c->qcv, NULL);
    pthread_mutex_init(&c->pklock, NULL);
    pthread_mutex_init(&c->loadlock, NULL);
    s2_text_init(&c->text, c->arena + FN_R_DATA);
    s2_bus_reset(&c->bus);
    s_cart = c;
    return c;
}

void studio2_cart_destroy(studio2_cart_t *c)
{
    if (!c)
        return;
    studio2_cart_power_off(c);
    pthread_mutex_destroy(&c->qlock);
    pthread_cond_destroy(&c->qcv);
    pthread_mutex_destroy(&c->pklock);
    pthread_mutex_destroy(&c->loadlock);
    free(c->buf[0]);
    free(c->buf[1]);
    if (s_cart == c)
        s_cart = NULL;
    free(c);
}

static const char *plan_error(int err)
{
    switch (err)
    {
    case S2MAP_ETOOBIG:  return "the image is larger than the cartridge can serve (16,640 bytes)";
    case S2MAP_EEMPTY:   return "the image is empty";
    case S2MAP_EBADPAGE: return "the FujiNet app names a page the cartridge may not claim";
    default:             return "the cartridge cannot plan this image";
    }
}

int studio2_cart_check_image(const uint8_t *image, uint32_t len, bool *claims,
                             char *why, int whysz)
{
    s2map_plan_t plan;
    int err;

    if (claims)
        *claims = false;
    if (!image || len == 0)
        err = S2MAP_EEMPTY;
    else if (len > S2MAP_BUF_MAX)
        err = S2MAP_ETOOBIG;
    else
    {
        /* s2map_plan reads only; it never writes the buffer */
        err = s2map_plan(image, len, false, &plan);
    }
    if (err != S2MAP_OK)
    {
        if (why && whysz > 0)
            snprintf(why, (size_t)whysz, "%s", plan_error(err));
        return 0;
    }
    if (claims)
        *claims = plan.claim;
    if (why && whysz > 0)
        why[0] = '\0';
    return 1;
}

int studio2_cart_power_on(studio2_cart_t *c, const uint8_t *image, uint32_t len,
                          const char *boip, bool sync, char *why, int whysz)
{
    const char *env;

    if (c->powered)
        studio2_cart_power_off(c);
    if (why && whysz > 0)
        why[0] = '\0';
    if (image && len && !studio2_cart_check_image(image, len, NULL, why, whysz))
        return -1;

    /* device_start */
    c->debug = getenv("FUJINET_DEBUG") != NULL;
    env = getenv("FUJINET_SYNC");
    c->async = !(sync || (env && *env && *env != '0'));
    c->have_boip = boip != NULL;
    if (boip)
        snprintf(c->boip, sizeof c->boip, "%s", boip);

    /* device_reset */
    memset(c->arena, 0, FN_H_REGSEL);
    memset(c->arena + FN_H_REGSEL, 0xFF, FN_ARENA_SIZE - FN_H_REGSEL);
    c->arena[FN_H_REGSEL + FN_HOT_STUB] = 0xEC;
    c->arena[FN_H_REGSEL + FN_HOT_STUB + 1] = 0x70;
    c->arena[FN_H_REGSEL + FN_HOT_T] = 0x23;
    s2_text_init(&c->text, c->arena + FN_R_DATA);
    fuji_load_init(&c->load, c->buf[0], c->buf[1], c->arena, c->text.raster,
                   _configrom, FUJI_CONFIGROM_SIZE);
    s2_bus_reset(&c->bus);
    c->own_sel = 0xFF;
    c->short_lines = 0;
    c->rline = c->rcol = 0;
    c->npokes = 0;
    atomic_store(&c->poked, false);
    atomic_store(&c->busy, false);
    c->push = NULL;
    c->direct = false;
    t_on_worker = false;

    if (image && len)
    {
        /* served at power-on as if the cart had planned and swapped it in */
        uint8_t *t = fuji_load_target(&c->load);
        uint32_t n = len < S2MAP_BUF_MAX ? len : S2MAP_BUF_MAX;

        memcpy(t, image, n);
        uint8_t err = fuji_load_commit(&c->load, n);
        if (err)
        {
            if (why && whysz > 0)
                snprintf(why, (size_t)whysz, "the cartridge refused the image (%u)", err);
            return -1;
        }
        fuji_load_arm(&c->load);
        (void)fuji_load_swap(&c->load);
        c->direct = true;
        if (c->debug)
        {
            const s2map_plan_t *p = &c->load.plan[c->load.live];
            fprintf(stderr, "fujinet: DIRECT %u bytes, %s, %u blocks%s\n", (unsigned)n,
                    p->st2 ? "ST2" : "raw", p->blocks, p->claim ? ", claimed" : "");
        }
    }
    c->view = fuji_load_live(&c->load);
    c->mailbox_mode = c->view->mailbox;
    fujitcp_close();                    /* fujinet-pc's BoIP port takes one client */
    c->powered = true;
    if (c->mailbox_mode)
    {
        fujimail_init(c->debug ? &debug_port : &quiet_port);
        fujitcp_init(c->have_boip ? c->boip : NULL);
        fujimail_paint();
        publish_link(c);
        worker_start(c);
    }
    else
    {
        c->async = false;
    }
    return 0;
}

void studio2_cart_power_off(studio2_cart_t *c)
{
    if (!c || !c->powered)
        return;
    worker_stop(c);
    fujitcp_close();
    c->powered = false;
}

/*-------------------------------------------------
    status and debugger access
-------------------------------------------------*/

static uint32_t count24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

void studio2_cart_status(studio2_cart_t *c, studio2_cart_status_t *out)
{
    memset(out, 0, sizeof *out);
    if (!c)
        return;
    out->powered = c->powered;
    if (!c->powered)
        return;
    drain_pokes(c);
    out->mailbox_mode = c->mailbox_mode;
    out->link_up = fujitcp_active();
    out->busy = atomic_load(&c->busy);
    out->direct = c->direct;

    pthread_mutex_lock(&c->loadlock);
    const s2map_plan_t *lp = &c->load.plan[c->load.live];
    const s2map_plan_t *sp = &c->load.plan[c->load.live ^ 1u];
    out->live_mailbox = c->load.view[c->load.live].mailbox;
    out->live_st2 = lp->st2;
    out->live_blocks = lp->blocks;
    out->live_size = lp->size;
    out->live_crc = lp->crc;
    out->staged = c->load.staged;
    out->staged_claim = c->load.staged && sp->claim;
    out->staged_config = c->load.staged_config;
    out->staged_crc = sp->crc;
    out->armed = c->load.armed;
    out->swaps = c->load.swaps;
    pthread_mutex_unlock(&c->loadlock);

    out->booted_game = c->direct || out->swaps > 0;
    out->mode = c->mailbox_mode ? c->arena[FN_R_MODE]
              : FN_MODE_GAME;
    out->boot_state = c->arena[FN_R_BOOT_STATE];
    out->boot_pct = c->arena[FN_R_BOOT_PCT];
    out->boot_err = c->arena[FN_R_BOOT_ERR];
    out->boot_got = count24(c->arena + FN_R_BOOT_GOT0);
    out->boot_total = count24(c->arena + FN_R_BOOT_TOT0);
    out->ackseq = c->arena[FN_R_ACKSEQ];
    out->status = c->arena[FN_R_STATUS];
    out->err = c->arena[FN_R_ERR];
    out->reply_cmd = c->arena[FN_R_REPLY_CMD];
    out->rxlen = (uint16_t)(c->arena[FN_R_RXLEN_LO] | (c->arena[FN_R_RXLEN_HI] << 8));
    out->frames = c->bus.frames;
    out->short_lines = c->short_lines;
    pthread_mutex_lock(&c->qlock);
    out->queue = queue_depth(c);
    pthread_mutex_unlock(&c->qlock);
}

void studio2_cart_page_types(studio2_cart_t *c, uint8_t out[256])
{
    if (!c || !c->powered)
    {
        memset(out, S2PG_NONE, 256);
        return;
    }
    memcpy(out, c->view->type, 256);
}

void studio2_cart_text(studio2_cart_t *c, studio2_cart_text_t *out)
{
    memset(out, 0, sizeof *out);
    if (!c || !c->powered)
        return;
    memcpy(out->chars, c->text.chars, sizeof out->chars);
    memcpy(out->inv, c->text.inv, sizeof out->inv);
    out->row = c->text.row;
    out->col = c->text.col;
    out->inverse = c->text.inverse;
}

uint32_t studio2_cart_copy_arena(studio2_cart_t *c, uint8_t *dst, uint32_t max)
{
    uint32_t n = max < FN_ARENA_SIZE ? max : FN_ARENA_SIZE;
    if (!c)
        return 0;
    drain_pokes(c);
    memcpy(dst, c->arena, n);
    return n;
}

uint32_t studio2_cart_copy_raster(studio2_cart_t *c, uint8_t *dst, uint32_t max)
{
    uint32_t n = max < FN_RASTER_SIZE ? max : FN_RASTER_SIZE;
    if (!c)
        return 0;
    memcpy(dst, c->text.raster, n);
    return n;
}
