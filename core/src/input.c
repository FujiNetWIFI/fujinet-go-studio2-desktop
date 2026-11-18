/*
 * input.c -- the merged input layer: who holds each keypad key and CLEAR.
 *
 * Every source holds a target by its own bit (session_internal.h
 * STUDIO2_SRC_*): the keyboard, the Controllers window's pointer, and for each
 * gamepad its buttons and its stick separately. The machine sees the union,
 * so one source letting go never releases another's hold (a panel release
 * used to clear a keyboard hold of the same key, and two pads on one keypad
 * cancelled each other). All of it is under the session's input_mtx, a leaf
 * lock: the UI thread and the gamepad thread both write here. Ported from the
 * Channel F / Astrocade siblings' input.c.
 *
 * CLEAR holds the CPU in reset while down, so a source that fails to let go
 * freezes the machine: pointer and pad holds are released by their own entry
 * points (studio2session_pointer_release, session_gamepad_release) and only
 * those, never by another source's release.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "host.h"
#include "session_internal.h"

/* Recompute what the machine sees from the bits. input_mtx held. */
static void push_locked(struct studio2session *s)
{
    int port, act, sw;

    if (!s->running)
        return;
    for (port = 0; port < 2; port++) {
        uint16_t mask = 0;
        for (act = 0; act < STUDIO2_ACT_PER_PORT; act++)
            if (s->pad_src[port][act])
                mask |= (uint16_t)(1u << act);
        studio2_host_keypad_set(port, mask);
    }
    for (sw = 0; sw < STUDIO2_SW_COUNT; sw++)
        if (sw == STUDIO2_SW_CLEAR)
            studio2_host_clear_set(s->sw_src[sw] != 0);
}

static uint32_t *slot_locked(struct studio2session *s, int target)
{
    if (target >= 0 && target < 2 * STUDIO2_ACT_PER_PORT)
        return &s->pad_src[target / STUDIO2_ACT_PER_PORT][target % STUDIO2_ACT_PER_PORT];
    target -= 2 * STUDIO2_ACT_PER_PORT;
    if (target >= 0 && target < STUDIO2_SW_COUNT)
        return &s->sw_src[target];
    return NULL;
}

void input_source(struct studio2session *s, int target, uint32_t src, int down)
{
    uint32_t *slot;

    pthread_mutex_lock(&s->input_mtx);
    slot = slot_locked(s, target);
    /* a press needs a running machine; a release always lets go */
    if (slot && (!down || s->running)) {
        if (down)
            *slot |= src;
        else
            *slot &= ~src;
        push_locked(s);
    }
    pthread_mutex_unlock(&s->input_mtx);
}

void input_clear_sources(struct studio2session *s, uint32_t mask)
{
    int port, act, sw;

    pthread_mutex_lock(&s->input_mtx);
    for (port = 0; port < 2; port++)
        for (act = 0; act < STUDIO2_ACT_PER_PORT; act++)
            s->pad_src[port][act] &= ~mask;
    for (sw = 0; sw < STUDIO2_SW_COUNT; sw++)
        s->sw_src[sw] &= ~mask;
    push_locked(s);
    pthread_mutex_unlock(&s->input_mtx);
}

void input_resync(struct studio2session *s)
{
    pthread_mutex_lock(&s->input_mtx);
    push_locked(s);
    pthread_mutex_unlock(&s->input_mtx);
}

/* ---- public: the Controllers window's pointer ------------------------------- */

void studio2session_pointer_press(studio2session *s, int target, int down)
{
    if (!s || target < 0 || target >= STUDIO2_TARGET_COUNT)
        return;
    if (target >= STUDIO2_TARGET_SYSACT(0)) {
        /* a system action fires on the press, never held */
        if (down && s->running)
            studio2session_sysaction(s, target - STUDIO2_TARGET_SYSACT(0));
        return;
    }
    input_source(s, target, STUDIO2_SRC_POINTER, down != 0);
}

void studio2session_press(studio2session *s, int target, int down)
{
    studio2session_pointer_press(s, target, down);
}

void studio2session_release_pointer(studio2session *s)
{
    if (s)
        input_clear_sources(s, STUDIO2_SRC_POINTER);
}

void studio2session_release_all(studio2session *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->input_mtx);
    memset(s->held_keysym, 0, sizeof s->held_keysym);
    pthread_mutex_unlock(&s->input_mtx);
    input_clear_sources(s, STUDIO2_SRC_KEYBOARD);
}

unsigned studio2session_buttons_held(studio2session *s, int port)
{
    unsigned held = 0;
    int act;

    if (!s || port < 0 || port > 1)
        return 0;
    pthread_mutex_lock(&s->input_mtx);
    for (act = 0; act < STUDIO2_ACT_PER_PORT; act++)
        if (s->pad_src[port][act])
            held |= 1u << act;
    pthread_mutex_unlock(&s->input_mtx);
    return held;
}

int studio2session_switch_held(studio2session *s, int sw)
{
    int held;

    if (!s || sw < 0 || sw >= STUDIO2_SW_COUNT)
        return 0;
    pthread_mutex_lock(&s->input_mtx);
    held = s->sw_src[sw] != 0;
    pthread_mutex_unlock(&s->input_mtx);
    return held;
}

int studio2session_target_held(studio2session *s, int target)
{
    if (target >= 0 && target < 2 * STUDIO2_ACT_PER_PORT)
        return (studio2session_buttons_held(s, target / STUDIO2_ACT_PER_PORT)
                >> (target % STUDIO2_ACT_PER_PORT)) & 1;
    if (target >= 2 * STUDIO2_ACT_PER_PORT && target < STUDIO2_TARGET_SYSACT(0))
        return studio2session_switch_held(s, target - 2 * STUDIO2_ACT_PER_PORT);
    return 0;
}

/* ---- the gamepad thread's entry points -------------------------------------- */

void session_gamepad_apply(struct studio2session *s, int owner, int port, int act, int down)
{
    if (owner < 0 || owner >= STUDIO2_MAX_PADS || port < 0 || port > 1 ||
        act < 0 || act >= STUDIO2_ACT_PER_PORT)
        return;
    input_source(s, STUDIO2_TARGET_PORT(port, act), STUDIO2_SRC_STICK(owner), down != 0);
}

void session_gamepad_target(struct studio2session *s, int owner, int target, int down)
{
    if (owner < 0 || owner >= STUDIO2_MAX_PADS || target < 0 ||
        target >= STUDIO2_TARGET_SYSACT(0))
        return;
    input_source(s, target, STUDIO2_SRC_PAD(owner), down != 0);
}

void session_gamepad_release(struct studio2session *s, int owner)
{
    if (owner < 0 || owner >= STUDIO2_MAX_PADS)
        return;
    input_clear_sources(s, STUDIO2_SRC_PAD(owner) | STUDIO2_SRC_STICK(owner));
}
