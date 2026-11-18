/*
 * gamepad_sdl -- SDL3 gamepads (and unmapped joysticks) as Studio II keypads,
 * on their own polling thread.
 *
 * A polling thread rather than SDL's event queue on the UI thread: this
 * app's video belongs to GTK/Qt/AppKit/Win32, not SDL, and only the gamepad
 * subsystem is initialised here -- never SDL video. Devices are added and
 * removed as they appear on the system: SDL posts the JOYSTICK_ and
 * GAMEPAD_ add/remove pairs and both are taken, with idempotent open/close,
 * so neither kind of device can be missed. A device SDL has no gamepad
 * mapping for (a plain HID adapter) is opened as a raw joystick instead:
 * its buttons and hats are bindable in the raw bands, just unnamed.
 *
 * What a pad drives is its keypad (A for the first, B for the second): the
 * D-pad (and, with "analog sticks" on, the left stick as an eight-way switch
 * with hysteresis) is the keys the Studio II's games steer with -- 2 4 6 8,
 * with a diagonal on its corner key, 1 3 7 9, not on two keys at once; the
 * buttons come from the bindings table.
 *
 * Buttons go through the bindings table (a keypad's keys, the console's
 * CLEAR, the system actions); a D-pad direction the user has bound to
 * something explicit stops also nudging the directional role. Ported from
 * the SMS sibling's gamepad_sdl.c.
 *
 * Pads are assigned to ports in connection order unless assigned explicitly.
 * An explicit assignment is remembered (settings key "pad_assign", see
 * pad_assign.h): a pad that is unplugged and plugged back in, or is there on
 * the next run, gets it back -- matched by its SDL GUID, else by its name. Two
 * identical pads share one entry; the first to connect gets it. Every
 * connect/disconnect is reported to the session for the frontend's toast.
 *
 * Each pad has a stable owner number (0..MAX_PADS-1) while connected, and its
 * buttons and its stick/D-pad hold keypad keys as separate sources in input.c,
 * so two pads on one keypad, or a pad and the keyboard, never cancel each
 * other.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gamepad_sdl.h"
#include "pad_assign.h"
#include "session_internal.h"

#define MAX_PADS STUDIO2_MAX_PADS
#define MAX_HATS 4

/* A third of full deflection engages, a sixth releases: low enough that a
 * worn stick still registers, high enough that resting drift does not, and
 * a gap wide enough that jitter across one threshold cannot flutter. */
#define STICK_ENTER 0.35f
#define STICK_EXIT  0.15f
#define TRIGGER_ON  0.5f

typedef struct {
    SDL_JoystickID id;
    int owner;                /* input.c source number: stable while connected */
    SDL_Gamepad *handle;      /* mapped gamepad, or NULL */
    SDL_Joystick *joy;        /* raw joystick when not a gamepad */
    int assign;               /* explicit port, -1 = automatic */
    int by_name;              /* assign was restored from a name-only match
                               * (the entry's GUID differs): an exact-GUID
                               * pad may take it over */
    int port;                 /* the port it drove on the last poll, or -1 */
    int stick_dir;            /* left stick's four-way state (hysteresis) */
    int dpad_dir;             /* D-pad / hat 0 state */
    uint8_t sent[STUDIO2_ACT_PER_PORT]; /* directional actions currently held */
    /* What each button's press asserted (target + 1, 0 = nothing), so its
     * release lets go of exactly that even if the binding changed meanwhile. */
    int16_t btn_target[256];
    SDL_GUID guid;
    char guidstr[STUDIO2_PAD_GUID_MAX];   /* guid as hex, for the pad_assign table */
    char name[STUDIO2_PAD_NAME_MAX];
    int trigger_down[2];
    Uint8 last_hat[MAX_HATS];
} pad_slot;

typedef struct {
    struct studio2session *session;
    pthread_t thread;
    pthread_mutex_t lock;
    int running;
    atomic_int stop_requested;
    pad_slot pads[MAX_PADS];
    int npads;
    unsigned generation;
    /* Map-mode capture */
    int cap_active, cap_have, cap_result;
    /* the remembered explicit assignments (the "pad_assign" setting), for
     * pads that are away as well as those here; guarded by lock */
    studio2_pad_table table;
} gamepad_state;

static gamepad_state *g_state;   /* one per process: SDL's subsystem is */

/* ---- pure helpers ----------------------------------------------------------- */

int studio2_key_from_dir(int dir)
{
    /* rows top to bottom, columns left to right, as on the keypad */
    static const int keys[3][3] = { { 1, 2, 3 }, { 4, 5, 6 }, { 7, 8, 9 } };
    int row = 1, col = 1;

    if ((dir & STUDIO2_DIR_UP) && !(dir & STUDIO2_DIR_DOWN)) row = 0;
    if ((dir & STUDIO2_DIR_DOWN) && !(dir & STUDIO2_DIR_UP)) row = 2;
    if ((dir & STUDIO2_DIR_LEFT) && !(dir & STUDIO2_DIR_RIGHT)) col = 0;
    if ((dir & STUDIO2_DIR_RIGHT) && !(dir & STUDIO2_DIR_LEFT)) col = 2;
    return keys[row][col];
}

int studio2_dir_from_stick(float x, float y, int prev, float enter, float exit)
{
    int dir = 0;
    /* each axis independently, so diagonals are two bits and a stick that
     * is exactly horizontal never picks up a vertical bit from noise */
    if (x <= -enter || (x <= -exit && (prev & STUDIO2_DIR_LEFT))) dir |= STUDIO2_DIR_LEFT;
    if (x >=  enter || (x >=  exit && (prev & STUDIO2_DIR_RIGHT))) dir |= STUDIO2_DIR_RIGHT;
    if (y <= -enter || (y <= -exit && (prev & STUDIO2_DIR_UP))) dir |= STUDIO2_DIR_UP;
    if (y >=  enter || (y >=  exit && (prev & STUDIO2_DIR_DOWN))) dir |= STUDIO2_DIR_DOWN;
    return dir;
}

int studio2_pad_for_port(const int *assign, int npads, int port)
{
    int i, next = 0;
    for (i = 0; i < npads; i++)
        if (assign[i] == port) return i;
    for (i = 0; i < npads; i++) {
        if (assign[i] >= 0) continue;   /* explicitly elsewhere */
        if (next == port) return i;
        next++;
    }
    return -1;
}

int studio2_pad_button_from_sdl(uint8_t button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return STUDIO2_PAD_BTN_SOUTH;
    case SDL_GAMEPAD_BUTTON_EAST: return STUDIO2_PAD_BTN_EAST;
    case SDL_GAMEPAD_BUTTON_WEST: return STUDIO2_PAD_BTN_WEST;
    case SDL_GAMEPAD_BUTTON_NORTH: return STUDIO2_PAD_BTN_NORTH;
    case SDL_GAMEPAD_BUTTON_BACK: return STUDIO2_PAD_BTN_BACK;
    case SDL_GAMEPAD_BUTTON_GUIDE: return STUDIO2_PAD_BTN_GUIDE;
    case SDL_GAMEPAD_BUTTON_START: return STUDIO2_PAD_BTN_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return STUDIO2_PAD_BTN_LEFT_STICK;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return STUDIO2_PAD_BTN_RIGHT_STICK;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return STUDIO2_PAD_BTN_LEFT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return STUDIO2_PAD_BTN_RIGHT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return STUDIO2_PAD_BTN_DPAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return STUDIO2_PAD_BTN_DPAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return STUDIO2_PAD_BTN_DPAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return STUDIO2_PAD_BTN_DPAD_RIGHT;
    default: return STUDIO2_PAD_BTN_NONE;
    }
}

int studio2_sdl_button_from_pad(int button)
{
    switch (button) {
    case STUDIO2_PAD_BTN_SOUTH: return SDL_GAMEPAD_BUTTON_SOUTH;
    case STUDIO2_PAD_BTN_EAST: return SDL_GAMEPAD_BUTTON_EAST;
    case STUDIO2_PAD_BTN_WEST: return SDL_GAMEPAD_BUTTON_WEST;
    case STUDIO2_PAD_BTN_NORTH: return SDL_GAMEPAD_BUTTON_NORTH;
    case STUDIO2_PAD_BTN_BACK: return SDL_GAMEPAD_BUTTON_BACK;
    case STUDIO2_PAD_BTN_GUIDE: return SDL_GAMEPAD_BUTTON_GUIDE;
    case STUDIO2_PAD_BTN_START: return SDL_GAMEPAD_BUTTON_START;
    case STUDIO2_PAD_BTN_LEFT_STICK: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case STUDIO2_PAD_BTN_RIGHT_STICK: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case STUDIO2_PAD_BTN_LEFT_SHOULDER: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case STUDIO2_PAD_BTN_RIGHT_SHOULDER: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case STUDIO2_PAD_BTN_DPAD_UP: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case STUDIO2_PAD_BTN_DPAD_DOWN: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case STUDIO2_PAD_BTN_DPAD_LEFT: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case STUDIO2_PAD_BTN_DPAD_RIGHT: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    default: return -1;
    }
}

/* ---- slots ------------------------------------------------------------------ */

static int slot_for_id(gamepad_state *g, SDL_JoystickID id)
{
    int i;
    for (i = 0; i < g->npads; i++)
        if (g->pads[i].id == id) return i;
    return -1;
}

static const char *slot_name(const pad_slot *slot)
{
    const char *n = slot->handle ? SDL_GetGamepadName(slot->handle)
                                 : SDL_GetJoystickName(slot->joy);
    return n ? n : "Gamepad";
}

/* Lets go of everything the pad holds, whichever way it was held. The caller
 * may hold g->lock: input.c's lock is a leaf below it. */
static void slot_release_all(gamepad_state *g, pad_slot *slot)
{
    memset(slot->sent, 0, sizeof slot->sent);
    memset(slot->btn_target, 0, sizeof slot->btn_target);
    session_gamepad_release(g->session, slot->owner);
}

static void slot_close(gamepad_state *g, pad_slot *slot)
{
    slot_release_all(g, slot);
    if (slot->handle) SDL_CloseGamepad(slot->handle);
    if (slot->joy) SDL_CloseJoystick(slot->joy);
    slot->handle = NULL;
    slot->joy = NULL;
}

static int effective_port_locked(gamepad_state *g, int idx);

/* Gives the pad in slot idx its remembered port. An entry with the pad's own
 * GUID is the pad's: it takes it from a pad that got it only through the name
 * fallback. A pad known only by name takes the entry if no other pad carries
 * it (two identical pads, or one pad known under two GUIDs): then this one
 * stays automatic. Locked by the caller; only reads g->table. */
static void restore_assignment(gamepad_state *g, int idx)
{
    pad_slot *slot = &g->pads[idx];
    int ent = studio2_pad_table_find(&g->table, slot->guidstr, slot->name), kix;
    int exact;

    if (ent < 0 || g->table.entry[ent].port >= 2) return;
    exact = !strcmp(g->table.entry[ent].guid, slot->guidstr);
    for (kix = 0; kix < g->npads; kix++) {
        pad_slot *other = &g->pads[kix];
        if (kix == idx || other->assign < 0 ||
            studio2_pad_table_find(&g->table, other->guidstr, other->name) != ent)
            continue;
        if (!exact || !other->by_name) return;
        other->assign = -1;          /* the entry's owner is here: it wins */
        other->by_name = 0;
    }
    slot->assign = g->table.entry[ent].port;
    slot->by_name = !exact;
}

/* Idempotent, and the single place the gamepad/joystick decision is made:
 * both SDL_EVENT_JOYSTICK_ADDED and SDL_EVENT_GAMEPAD_ADDED land here (SDL
 * posts both for a mapped device) and the slot_for_id guard makes the second
 * one a no-op. */
static void open_pad(gamepad_state *g, SDL_JoystickID id)
{
    pthread_mutex_lock(&g->lock);
    if (g->npads < MAX_PADS && slot_for_id(g, id) < 0) {
        /* Not an else-if on SDL_IsGamepad: a device SDL calls a gamepad can
         * still fail to open as one. Falling through to the raw joystick
         * keeps it usable -- every button is still bindable, just unnamed. */
        SDL_Gamepad *gp = SDL_IsGamepad(id) ? SDL_OpenGamepad(id) : NULL;
        SDL_Joystick *j = gp ? NULL : SDL_OpenJoystick(id);
        if (gp || j) {
            pad_slot *slot = &g->pads[g->npads++];
            int h, used = 0, kix;
            for (kix = 0; kix < g->npads - 1; kix++)
                used |= 1 << g->pads[kix].owner;
            memset(slot, 0, sizeof *slot);
            for (kix = 0; kix < MAX_PADS && (used & (1 << kix)); kix++) {}
            slot->owner = kix;
            slot->id = id;
            slot->handle = gp;
            slot->joy = j;
            slot->assign = -1;
            slot->port = -1;
            for (h = 0; h < MAX_HATS; h++) slot->last_hat[h] = SDL_HAT_CENTERED;
            slot->guid = SDL_GetJoystickGUIDForID(id);
            snprintf(slot->name, sizeof slot->name, "%s", slot_name(slot));
            SDL_GUIDToString(slot->guid, slot->guidstr, sizeof slot->guidstr);
            restore_assignment(g, g->npads - 1);
            g->generation++;
            fprintf(stderr, "studio2 gamepad: %s connected (%s)\n", slot->name,
                    gp ? "gamepad" : "raw joystick");
            {
                char msg[128];
                int port = effective_port_locked(g, g->npads - 1);
                if (port >= 0)
                    snprintf(msg, sizeof msg, "Connected: %s (keypad %c)", slot->name, 'A' + port);
                else
                    snprintf(msg, sizeof msg, "Connected: %s", slot->name);
                session_gamepad_event(g->session, msg);
            }
        }
    }
    pthread_mutex_unlock(&g->lock);
}

static void close_pad(gamepad_state *g, SDL_JoystickID id)
{
    int idx, i;
    pthread_mutex_lock(&g->lock);
    idx = slot_for_id(g, id);
    if (idx >= 0) {
        pad_slot *slot = &g->pads[idx];
        char msg[128];
        fprintf(stderr, "studio2 gamepad: %s disconnected\n", slot->name);
        snprintf(msg, sizeof msg, "Disconnected: %s", slot->name);
        slot_close(g, slot);
        session_gamepad_event(g->session, msg);
        for (i = idx; i < g->npads - 1; i++) g->pads[i] = g->pads[i + 1];
        g->npads--;
        g->generation++;
    }
    pthread_mutex_unlock(&g->lock);
}

/* ---- buttons ---------------------------------------------------------------- */

/* The port a slot drives right now. Locked by the caller. */
static int effective_port_locked(gamepad_state *g, int idx)
{
    int assign[MAX_PADS], i, port;
    for (i = 0; i < MAX_PADS; i++) assign[i] = -1;
    for (i = 0; i < g->npads; i++) assign[i] = g->pads[i].assign;
    for (port = 0; port < 2; port++)
        if (studio2_pad_for_port(assign, g->npads, port) == idx) return port;
    return -1;
}

static void dispatch_button(gamepad_state *g, SDL_JoystickID which, int b, int down)
{
    int idx, port, owner = 0, target = -1;
    if (b == STUDIO2_PAD_BTN_NONE) return;

    pthread_mutex_lock(&g->lock);
    if (g->cap_active) {
        /* Map mode: every button event is swallowed while armed, not just
         * the captured one, so the press that maps a button does not also
         * fire whatever it used to do. */
        if (down && !g->cap_have) { g->cap_have = 1; g->cap_result = b; }
        pthread_mutex_unlock(&g->lock);
        return;
    }
    idx = slot_for_id(g, which);
    port = idx >= 0 ? effective_port_locked(g, idx) : -1;
    if (idx >= 0 && b >= 0 && b < 256) {
        pad_slot *slot = &g->pads[idx];
        owner = slot->owner;
        if (down) {
            if (port >= 0) {
                target = studio2session_target_for_button(g->session, port, b);
                if (target >= 0 && target < STUDIO2_TARGET_SYSACT(0))
                    slot->btn_target[b] = (int16_t)(target + 1);
            }
        } else if (slot->btn_target[b]) {
            /* what the press asserted, whatever the binding says now */
            target = slot->btn_target[b] - 1;
            slot->btn_target[b] = 0;
        }
    }
    pthread_mutex_unlock(&g->lock);
    if (target < 0) return;

    if (target >= STUDIO2_TARGET_SYSACT(0)) {
        /* a console action is posted, on the press only, for the UI thread */
        if (down) studio2session_sysaction_post(g->session, target - STUDIO2_TARGET_SYSACT(0));
        return;
    }
    session_gamepad_target(g->session, owner, target, down);
}

static int hat_dir_from_mask(Uint8 mask)
{
    switch (mask) {
    case SDL_HAT_UP: return 0;
    case SDL_HAT_RIGHTUP: return 1;
    case SDL_HAT_RIGHT: return 2;
    case SDL_HAT_RIGHTDOWN: return 3;
    case SDL_HAT_DOWN: return 4;
    case SDL_HAT_LEFTDOWN: return 5;
    case SDL_HAT_LEFT: return 6;
    case SDL_HAT_LEFTUP: return 7;
    default: return -1;
    }
}

static void handle_hat_motion(gamepad_state *g, SDL_JoystickID which, int hat, Uint8 mask)
{
    int idx, prev_dir, dir;
    if (hat < 0 || hat >= MAX_HATS) return;
    pthread_mutex_lock(&g->lock);
    idx = slot_for_id(g, which);
    if (idx < 0 || g->pads[idx].handle) { pthread_mutex_unlock(&g->lock); return; }
    prev_dir = hat_dir_from_mask(g->pads[idx].last_hat[hat]);
    g->pads[idx].last_hat[hat] = mask;
    pthread_mutex_unlock(&g->lock);
    dir = hat_dir_from_mask(mask);
    /* hat positions are bindable buttons in their own band; hat 0's four
     * cardinal points also feed the directional role in poll_sticks */
    if (prev_dir >= 0)
        dispatch_button(g, which, STUDIO2_PAD_HAT_BASE + hat * STUDIO2_PAD_HAT_DIRS + prev_dir, 0);
    if (dir >= 0)
        dispatch_button(g, which, STUDIO2_PAD_HAT_BASE + hat * STUDIO2_PAD_HAT_DIRS + dir, 1);
}

/* ---- the directional role and the analog controllers ------------------------ */

static float axis_norm(Sint16 v)
{
    return v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f;
}

static void slot_stick(const pad_slot *slot, int right, float *x, float *y)
{
    if (slot->handle) {
        *x = axis_norm(SDL_GetGamepadAxis(slot->handle, right ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX));
        *y = axis_norm(SDL_GetGamepadAxis(slot->handle, right ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY));
    } else {
        int base = right ? 2 : 0;
        *x = SDL_GetNumJoystickAxes(slot->joy) > base ? axis_norm(SDL_GetJoystickAxis(slot->joy, base)) : 0.f;
        *y = SDL_GetNumJoystickAxes(slot->joy) > base + 1 ? axis_norm(SDL_GetJoystickAxis(slot->joy, base + 1)) : 0.f;
    }
}

static int slot_dpad(gamepad_state *g, const pad_slot *slot, int port)
{
    int dir = 0;
    if (slot->handle) {
        /* a D-pad direction the user bound explicitly no longer plays the
         * directional role -- otherwise a button remapped to a keypad key
         * would also nudge the joystick every poll */
        if (studio2session_target_for_button(g->session, port, STUDIO2_PAD_BTN_DPAD_UP) < 0
            && SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_UP)) dir |= STUDIO2_DIR_UP;
        if (studio2session_target_for_button(g->session, port, STUDIO2_PAD_BTN_DPAD_DOWN) < 0
            && SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) dir |= STUDIO2_DIR_DOWN;
        if (studio2session_target_for_button(g->session, port, STUDIO2_PAD_BTN_DPAD_LEFT) < 0
            && SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) dir |= STUDIO2_DIR_LEFT;
        if (studio2session_target_for_button(g->session, port, STUDIO2_PAD_BTN_DPAD_RIGHT) < 0
            && SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) dir |= STUDIO2_DIR_RIGHT;
    } else if (SDL_GetNumJoystickHats(slot->joy) > 0) {
        Uint8 m = SDL_GetJoystickHat(slot->joy, 0);
        if (m & SDL_HAT_UP) dir |= STUDIO2_DIR_UP;
        if (m & SDL_HAT_DOWN) dir |= STUDIO2_DIR_DOWN;
        if (m & SDL_HAT_LEFT) dir |= STUDIO2_DIR_LEFT;
        if (m & SDL_HAT_RIGHT) dir |= STUDIO2_DIR_RIGHT;
    }
    return dir;
}

static void want_action(uint8_t *want, int act, int on)
{
    if (on) want[act] = 1;
}

/* A trigger crossing its threshold. Dispatched by poll_sticks once it has
 * let go of the lock: dispatch_button takes it too, and it is not recursive. */
typedef struct {
    SDL_JoystickID id;
    int button;
    int down;
} trigger_edge;

#define MAX_EDGES (MAX_PADS * 2)

static void poll_one(gamepad_state *g, pad_slot *slot, int port,
                     trigger_edge *edges, int *nedges)
{
    const struct studio2session *s = g->session;
    uint8_t want[STUDIO2_ACT_PER_PORT];
    int dir, a;
    float lx, ly;

    memset(want, 0, sizeof want);
    if (port != slot->port) {
        /* moved to another port (or none): release what it held there */
        slot_release_all(g, slot);
        slot->port = port;
        slot->stick_dir = 0;
        if (port < 0) return;
    }

    slot->dpad_dir = slot_dpad(g, slot, port);
    slot_stick(slot, 0, &lx, &ly);

    dir = slot->dpad_dir;
    if (s->opts.analog_joystick) {
        slot->stick_dir = studio2_dir_from_stick(lx, ly, slot->stick_dir, STICK_ENTER, STICK_EXIT);
        dir |= slot->stick_dir;
    }
    {
        const int key = studio2_key_from_dir(dir);
        /* opposite directions together cancel to the centre: nothing */
        want_action(want, key, key != 5);
    }

    for (a = 0; a < STUDIO2_ACT_PER_PORT; a++) {
        if (want[a] != slot->sent[a]) {
            slot->sent[a] = want[a];
            session_gamepad_apply(g->session, slot->owner, port, a, want[a]);
        }
    }

    /* the triggers, as buttons */
    if (slot->handle) {
        int t;
        for (t = 0; t < 2; t++) {
            float v = axis_norm(SDL_GetGamepadAxis(slot->handle,
                t ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
            int down = v >= TRIGGER_ON;
            if (down != slot->trigger_down[t]) {
                slot->trigger_down[t] = down;
                if (*nedges < MAX_EDGES) {
                    edges[*nedges].id = slot->id;
                    edges[*nedges].button = t ? STUDIO2_PAD_BTN_RIGHT_TRIGGER : STUDIO2_PAD_BTN_LEFT_TRIGGER;
                    edges[*nedges].down = down;
                    (*nedges)++;
                }
            }
        }
    }
}

static void poll_sticks(gamepad_state *g)
{
    int i, nedges = 0;
    trigger_edge edges[MAX_EDGES];
    pthread_mutex_lock(&g->lock);
    for (i = 0; i < g->npads; i++)
        poll_one(g, &g->pads[i], effective_port_locked(g, i), edges, &nedges);
    pthread_mutex_unlock(&g->lock);
    for (i = 0; i < nedges; i++)
        dispatch_button(g, edges[i].id, edges[i].button, edges[i].down);
}

/* ---- the thread ------------------------------------------------------------- */

static void *thread_main(void *arg)
{
    gamepad_state *g = arg;
    int count = 0, i;
    /* Every joystick, not just the ones SDL has a gamepad mapping for:
     * SDL_GetGamepads would silently omit a plain HID adapter entirely. */
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (ids) {
        for (i = 0; i < count; i++) open_pad(g, ids[i]);
        SDL_free(ids);
    }

    while (!atomic_load(&g->stop_requested)) {
        SDL_Event ev;
        int npads;

        pthread_mutex_lock(&g->lock);
        npads = g->npads;
        pthread_mutex_unlock(&g->lock);
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_JOYSTICK_ADDED: open_pad(g, ev.jdevice.which); break;
            case SDL_EVENT_JOYSTICK_REMOVED: close_pad(g, ev.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_ADDED: open_pad(g, ev.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: close_pad(g, ev.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                dispatch_button(g, ev.gbutton.which,
                                studio2_pad_button_from_sdl(ev.gbutton.button),
                                ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP: {
                /* a mapped gamepad also emits these for the same press it
                 * reported as a GAMEPAD_BUTTON event; the slot's kind decides */
                int gamepad, idx;
                pthread_mutex_lock(&g->lock);
                idx = slot_for_id(g, ev.jbutton.which);
                gamepad = idx >= 0 && g->pads[idx].handle != NULL;
                pthread_mutex_unlock(&g->lock);
                if (gamepad || ev.jbutton.button >= 64) break;
                dispatch_button(g, ev.jbutton.which,
                                STUDIO2_PAD_BTN_RAW_BASE + ev.jbutton.button,
                                ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
                break;
            }
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                handle_hat_motion(g, ev.jhat.which, ev.jhat.hat, ev.jhat.value);
                break;
            default:
                break;
            }
        }
        poll_sticks(g);
        /* ~60 Hz while a pad is attached (the machine's own sampling rate).
         * With none there are only hotplug events to notice, which can wait
         * a tenth of a second. (Not SDL_WaitEventTimeout: SDL polls inside
         * it at about 1 kHz.) */
        SDL_Delay(npads > 0 ? 16 : 100);
    }

    pthread_mutex_lock(&g->lock);
    for (i = 0; i < g->npads; i++) slot_close(g, &g->pads[i]);
    g->npads = 0;
    pthread_mutex_unlock(&g->lock);
    return NULL;
}

int gamepad_start(struct studio2session *s)
{
    gamepad_state *g;
    if (s->gamepad) return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* Bluetooth pads over hidapi and the joystick subsystem together. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        session_set_error(s, "SDL gamepad init failed: %s", SDL_GetError());
        return -1;
    }
    g = calloc(1, sizeof *g);
    if (!g) { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); return -1; }
    g->session = s;
    pthread_mutex_init(&g->lock, NULL);
    /* the remembered assignments, once: garbage or no key is an empty table */
    studio2_pad_table_decode(&g->table, studio2session_get_str(s, "pad_assign", ""), 2);
    /* An explicit stack: the platform default is 512 KB on macOS, and SDL's
     * HID enumeration runs on this thread. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4u << 20);
    if (pthread_create(&g->thread, &attr, thread_main, g) != 0) {
        pthread_attr_destroy(&attr);
        session_set_error(s, "gamepad thread could not start");
        pthread_mutex_destroy(&g->lock);
        free(g);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    pthread_attr_destroy(&attr);
    g->running = 1;
    g_state = g;
    s->gamepad = g;
    return 0;
}

void gamepad_stop(struct studio2session *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    atomic_store(&g->stop_requested, 1);
    pthread_join(g->thread, NULL);
    pthread_mutex_destroy(&g->lock);
    s->gamepad = NULL;
    if (g_state == g) g_state = NULL;
    free(g);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

/* ---- public API ------------------------------------------------------------- */

int studio2session_gamepad_count(studio2session *s)
{
    gamepad_state *g = s->gamepad;
    int n;
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    n = g->npads;
    pthread_mutex_unlock(&g->lock);
    return n;
}

int studio2session_gamepad_name(studio2session *s, int idx, char *dst, int dstsz)
{
    gamepad_state *g = s->gamepad;
    int n = 0;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads)
        n = snprintf(dst, (size_t)dstsz, "%s", slot_name(&g->pads[idx]));
    pthread_mutex_unlock(&g->lock);
    return n;
}

void studio2session_gamepad_assign(studio2session *s, int idx, int port)
{
    gamepad_state *g = s->gamepad;
    char text[STUDIO2_PAD_ASSIGN_TEXT_MAX];
    int changed = 0;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) {
        pad_slot *slot = &g->pads[idx];
        if (port >= 0 && port < 2) {
            slot->assign = port;
            slot->by_name = 0;
            changed = studio2_pad_table_set(&g->table, slot->guidstr, slot->name, port) > 0;
        } else {
            const char *kguid[MAX_PADS], *kname[MAX_PADS];
            int kix, nkeep = 0;
            slot->assign = -1;
            slot->by_name = 0;
            for (kix = 0; kix < g->npads; kix++)
                if (kix != idx && g->pads[kix].assign >= 0) {
                    kguid[nkeep] = g->pads[kix].guidstr;
                    kname[nkeep++] = g->pads[kix].name;
                }
            changed = studio2_pad_table_remove(&g->table, slot->guidstr, slot->name,
                                               nkeep, kguid, kname);
        }
        if (changed && studio2_pad_table_encode(&g->table, text, sizeof text) < 0)
            changed = 0;
    }
    pthread_mutex_unlock(&g->lock);
    /* the settings file is written here, on the caller's thread, with the
     * pad lock let go: the pad thread must never wait on a file write */
    if (changed)
        studio2session_set_str(s, "pad_assign", text);
}

int studio2session_gamepad_assignment(studio2session *s, int idx)
{
    gamepad_state *g = s->gamepad;
    int r = -1;
    if (!g) return -1;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) r = g->pads[idx].assign;
    pthread_mutex_unlock(&g->lock);
    return r;
}

int studio2session_gamepad_effective_port(studio2session *s, int idx)
{
    gamepad_state *g = s->gamepad;
    int r = -1;
    if (!g) return -1;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) r = effective_port_locked(g, idx);
    pthread_mutex_unlock(&g->lock);
    return r;
}

unsigned studio2session_gamepad_generation(studio2session *s)
{
    gamepad_state *g = s->gamepad;
    unsigned r;
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    r = g->generation;
    pthread_mutex_unlock(&g->lock);
    return r;
}

void studio2session_gamepad_capture_begin(studio2session *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    g->cap_active = 1; g->cap_have = 0; g->cap_result = STUDIO2_PAD_BTN_NONE;
    pthread_mutex_unlock(&g->lock);
}

void studio2session_gamepad_capture_cancel(studio2session *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    g->cap_active = 0; g->cap_have = 0;
    pthread_mutex_unlock(&g->lock);
}

int studio2session_gamepad_capture_poll(studio2session *s, int *button)
{
    gamepad_state *g = s->gamepad;
    int got = 0;
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    if (g->cap_active && g->cap_have) {
        if (button) *button = g->cap_result;
        g->cap_active = 0; g->cap_have = 0;
        got = 1;
    }
    pthread_mutex_unlock(&g->lock);
    return got;
}
