/*
 * cart_test -- the FujiNet cartridge device and its transport against a
 * stand-in FujiNet on loopback, with no FujiNet runtime: the case the Linux
 * CI build (WITH_FUJINET=OFF) would otherwise never exercise.
 *
 * The console side is the bus itself: every mailbox write on the Studio II
 * is a cartridge READ whose address carries the payload -- reading $E905
 * selects register 5, reading $EA2A sets it to $2A -- so the test reads the
 * addresses a client would, through studio2_cart_read(), with the bus time
 * moving on as the console's would. The firmware's host tests cover the
 * protocol against a stub port; this covers the port: the transposed device,
 * the portable transport, the staging and the swap, and the debugger's trace
 * tap held to the bytes actually on the wire.
 *
 * The stand-in answers one MOUNT_IMAGE the way fujinet-pc's ROM media does:
 * a pushed stream (OPEN, WRITE, WRITE, CLOSE, each waiting for its ACK) and
 * then the reply. POSIX only (the loopback listener); SKIPs (77) elsewhere.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "fuji_mailbox.h"
#include "fujinet_cart.h"
#include "fujitcp_host.h"
#include "fujitrace.h"
#include "test_rom.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

#ifdef _WIN32
int main(void)
{
    printf("SKIP: the loopback stand-in is POSIX only\n");
    return 77;
}
#else

/* ---- the wire, as the trace saw it and as the stand-in did ---- */

typedef struct {
    uint8_t b[8192];
    size_t n;
} wire;

static wire traced_wire[2];     /* by the trace's dir: 0 read, 1 sent */
static fujitrace traced_ft;

static void keep_trace(void *user, int dir, const uint8_t *buf, size_t len)
{
    wire *w = &traced_wire[dir ? 1 : 0];
    (void)user;
    if (w->n + len <= sizeof w->b) {
        memcpy(w->b + w->n, buf, len);
        w->n += len;
    }
    fujitrace_feed(&traced_ft, dir, buf, len);
}

/* A FujiBus packet with no params, SLIP-framed, as FujiNet sends one. */
static size_t slip_frame(uint8_t dev, uint8_t cmd, const uint8_t *data,
                         size_t len, uint8_t *out)
{
    uint8_t pkt[1100];
    uint16_t ck = 0;
    size_t n = 6 + len, i, o = 0;

    pkt[0] = dev;
    pkt[1] = cmd;
    pkt[2] = (uint8_t)(n & 0xFF);
    pkt[3] = (uint8_t)(n >> 8);
    pkt[4] = 0;
    pkt[5] = 0;
    memcpy(pkt + 6, data, len);
    for (i = 0; i < n; i++) {
        ck = (uint16_t)(ck + pkt[i]);
        ck = (uint16_t)((ck >> 8) + (ck & 0xFF));
    }
    pkt[4] = (uint8_t)ck;
    out[o++] = 0xC0;
    for (i = 0; i < n; i++) {
        if (pkt[i] == 0xC0 || pkt[i] == 0xDB) {
            out[o++] = 0xDB;
            out[o++] = pkt[i] == 0xC0 ? 0xDC : 0xDD;
        } else {
            out[o++] = pkt[i];
        }
    }
    out[o++] = 0xC0;
    return o;
}

typedef struct {
    int listen_fd, fd;
    wire got, sent;             /* the stand-in's side of the wire */
    int ok;
} fake_fujinet;

static int fake_read_frame(fake_fujinet *f)
{
    int ends = 0;
    uint8_t c;
    while (ends < 2) {
        if (recv(f->fd, &c, 1, 0) != 1) return -1;
        if (f->got.n < sizeof f->got.b) f->got.b[f->got.n++] = c;
        if (c == 0xC0) ends++;
    }
    return 0;
}

static void fake_send(fake_fujinet *f, uint8_t dev, uint8_t cmd,
                      const uint8_t *data, size_t len)
{
    uint8_t frame[2400];
    size_t n = slip_frame(dev, cmd, data, len, frame);
    memcpy(f->sent.b + f->sent.n, frame, n);
    f->sent.n += n;
    send(f->fd, frame, n, 0);
}

static void *fake_main(void *arg)
{
    /* stream 0 (the image), 1024 bytes */
    static const uint8_t open_hdr[5] = { 0, 0x00, 0x04, 0x00, 0x00 };
    static const uint8_t reply[1] = { 0x00 };
    fake_fujinet *f = arg;

    f->fd = accept(f->listen_fd, NULL, NULL);
    if (f->fd < 0 || fake_read_frame(f) != 0) return NULL;      /* MOUNT_IMAGE */
    fake_send(f, 0xFF, 0x4F, open_hdr, sizeof open_hdr);
    if (fake_read_frame(f) != 0) return NULL;                   /* its ACK */
    fake_send(f, 0xFF, 0x57, test_counter_app, 512);
    if (fake_read_frame(f) != 0) return NULL;
    fake_send(f, 0xFF, 0x57, test_counter_app + 512, 512);
    if (fake_read_frame(f) != 0) return NULL;
    fake_send(f, 0xFF, 0x43, NULL, 0);
    if (fake_read_frame(f) != 0) return NULL;
    fake_send(f, 0x70, 0x06, reply, sizeof reply);
    f->ok = 1;
    return NULL;
}

/* ---- the console's side: reads, with the bus time moving on ---- */

static studio2_cart_t *cart;
static uint32_t t_us = 1000;

static uint8_t rd(uint16_t a)
{
    uint8_t d = 0xFF;
    t_us += 9;                  /* about two machine cycles at 1.76 MHz */
    studio2_cart_read(cart, a, t_us, &d);
    return d;
}

static void reg_write(uint8_t reg, uint8_t val)
{
    rd((uint16_t)(FN_ARENA_BASE + FN_H_REGSEL + reg));
    rd((uint16_t)(FN_ARENA_BASE + FN_H_REGDATA + val));
}

int main(void)
{
    fake_fujinet f;
    pthread_t th;
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    char boip[64], why[200];
    studio2_cart_status_t st;
    uint8_t claim[4];
    int one = 1;

    memset(&f, 0, sizeof f);
    f.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    setsockopt(f.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (f.listen_fd < 0 || bind(f.listen_fd, (struct sockaddr *)&a, sizeof a) != 0 ||
        listen(f.listen_fd, 1) != 0 || getsockname(f.listen_fd, (struct sockaddr *)&a, &al) != 0) {
        printf("SKIP: no loopback listener here\n");
        return 77;
    }
    snprintf(boip, sizeof boip, "127.0.0.1:%d", ntohs(a.sin_port));
    pthread_create(&th, NULL, fake_main, &f);

    fujitrace_init(&traced_ft);
    fujitcp_set_trace(keep_trace, NULL);

    cart = studio2_cart_create();
    check(cart != NULL, "the cart creates");
    check(studio2_cart_power_on(cart, NULL, 0, boip, true, why, sizeof why) == 0,
          "it powers on with CONFIG, the mailbox inline");
    studio2_cart_status(cart, &st);
    check(st.mailbox_mode && st.link_up, "and dials the stand-in");
    {
        uint8_t m0 = rd(FN_ARENA_BASE + FN_R_MAGIC0), m1 = rd(FN_ARENA_BASE + FN_R_MAGIC1);
        check(m0 == 'F' && m1 == 'N', "the status page carries the cart's magic");
    }

    /* MOUNT_IMAGE: device $70, command $F8, no parameters, then SEQ */
    reg_write(FN_REG_DEVICE, 0x70);
    reg_write(FN_REG_CMD, 0xF8);
    reg_write(FN_REG_NPARAM, 0);
    reg_write(FN_REG_DATA_RST, 0);
    reg_write(FN_REG_SEQ, 1);
    pthread_join(th, NULL);
    check(f.ok, "the stand-in saw the request, pushed the image, and replied");
    studio2_cart_frame(cart);
    studio2_cart_status(cart, &st);
    check(st.ackseq == 1 && st.reply_cmd == 0x06, "ACKSEQ answers SEQ, with an ACK");
    check(st.boot_state == FN_BOOT_READY && st.boot_got == 1024 && st.boot_total == 1024,
          "the push is staged: READY, 1024 of 1024 bytes");
    check(st.staged && st.staged_claim && !st.armed, "a FujiNet app, staged, not yet armed");

    /* BOOTLOCK, then the hand-over: the stub's last read, then $0000 */
    reg_write(FN_REG_BOOTLOCK, FN_BOOTLOCK_MAGIC);
    studio2_cart_status(cart, &st);
    check(st.armed, "BOOTLOCK arms the swap");
    rd(FN_STUB);
    rd(FN_STUB + 1);
    rd(FN_STUB_T);
    rd(0x0000);
    studio2_cart_status(cart, &st);
    check(st.swaps == 1 && st.live_mailbox && st.live_size == 1024,
          "the read of $0000 after the stub swaps it in");
    for (int i = 0; i < 4; i++)
        studio2_cart_peek(cart, (uint16_t)(FN_CLAIM_ADDR + i), &claim[i]);
    check(memcmp(claim, "FUJI", 4) == 0, "and the cart serves it");

    /* the trace, held to the wire */
    check(traced_wire[1].n == f.got.n && memcmp(traced_wire[1].b, f.got.b, f.got.n) == 0,
          "the trace's sent bytes are exactly what the stand-in received");
    check(traced_wire[0].n == f.sent.n && memcmp(traced_wire[0].b, f.sent.b, f.sent.n) == 0,
          "the trace's read bytes are exactly what the stand-in sent");
    {
        static studio2debug_fuji_packet p[64];
        int n = fujitrace_read(&traced_ft, p, 64);
        check(n >= 6, "the decoder made packets of it");
        check(n > 0 && !p[0].to_host && p[0].device == 0x70 && p[0].command == 0xF8,
              "the first is the console's MOUNT_IMAGE");
    }

    fujitcp_set_trace(NULL, NULL);
    studio2_cart_destroy(cart);
    close(f.listen_fd);
    if (f.fd >= 0)
        close(f.fd);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
#endif
