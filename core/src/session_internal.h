/*
 * studio2session's private state. Not installed; only the core/src sources
 * include it. One struct shared by the session's C modules (settings,
 * paths, media, roms, audio, gamepads, bindings).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STUDIO2_SESSION_INTERNAL_H
#define STUDIO2_SESSION_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "studio2session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STUDIO2_PATH_MAX 1024

/* Input sources (input.c): each holds a target by its own bit. */
#define STUDIO2_MAX_PADS     8
#define STUDIO2_SRC_KEYBOARD (1u << 0)
#define STUDIO2_SRC_POINTER  (1u << 1)
#define STUDIO2_SRC_PAD(o)   (1u << (2 + (o)))
#define STUDIO2_SRC_STICK(o) (1u << (2 + STUDIO2_MAX_PADS + (o)))

/* input.c: set or clear source `src` on a held target (keypad key or CLEAR);
 * the machine sees the union. A system action target is ignored. */
void input_source(struct studio2session *s, int target, uint32_t src, int down);
/* Clear the sources in `mask` from every target. */
void input_clear_sources(struct studio2session *s, uint32_t mask);
/* Push every merged value to the machine again (after it starts). */
void input_resync(struct studio2session *s);
typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

struct studio2session {
    char config_dir[STUDIO2_PATH_MAX];
    char data_dir[STUDIO2_PATH_MAX];
    char carts_dir[STUDIO2_PATH_MAX];
    char roms_dir[STUDIO2_PATH_MAX];       /* <data>/roms: the imported BIOS */
    char settings_file[STUDIO2_PATH_MAX];

    setting_kv *settings;
    pthread_mutex_t settings_mtx;
    int settings_dirty;

    char last_error[256];

    /* ---- FujiNet runtime (fujinet_runtime.c) ---- */
    char fujinet_root[STUDIO2_PATH_MAX];    /* <data>/fujinet */
    char fujinet_config[STUDIO2_PATH_MAX];  /* .../fnconfig.ini */
    char fujinet_sd[STUDIO2_PATH_MAX];      /* .../SD */
    char fujinet_data[STUDIO2_PATH_MAX];    /* .../data */
    char fujinet_lib[STUDIO2_PATH_MAX];     /* resolved libfujinet path, "" until then */
    int  fujinet_disabled;              /* the caller passed fujinet_lib "" */
    char fujinet_runtime_src[STUDIO2_PATH_MAX]; /* caller-given pristine tree, or "" */
    char webui_url[64];                 /* http://127.0.0.1:11519/ */
    int  fujinet_running;
    int  fujinet_external;              /* dialing a FujiNet that isn't ours */
    char boip_endpoint[80];             /* "host:port" the cartridge dials */
    char boip_addr[80];                 /* the external address as typed */

    /* cross-thread system-action latch (see studio2session_sysaction_post) */
    pthread_mutex_t sysact_mtx;
    unsigned sysact_pending;

    /* the running configuration */
    studio2session_start_opts opts;
    char cart_path[STUDIO2_PATH_MAX];

    /* keys the keyboard currently holds, by target, so a release clears
     * exactly what its press asserted even if the binding changed meanwhile */
    uint32_t held_keysym[STUDIO2_TARGET_COUNT];

    /* what drives each keypad key and console button: one STUDIO2_SRC_*
     * bit per source (the keyboard, the Controllers window's pointer, and
     * for each gamepad its buttons and its stick), under input_mtx (input.c),
     * so letting go of one source never releases another and the UI thread
     * and the gamepad thread never race */
    pthread_mutex_t input_mtx;
    uint32_t pad_src[2][STUDIO2_ACT_PER_PORT];
    uint32_t sw_src[STUDIO2_SW_COUNT];

    void *audio;              /* audio_sdl.c state, NULL until started */
    void *gamepad;            /* gamepad_sdl.c state, NULL until started */
    void *debugger;           /* studio2debug, lazily created */
    int running;

    /* the last gamepad hot-plug event, for a frontend toast */
    pthread_mutex_t pad_event_mtx;
    char pad_event[128];
};

void settings_init(struct studio2session *s);
void settings_free_all(struct studio2session *s);

int paths_init(struct studio2session *s, const char *config_dir,
               const char *data_dir);
/* The in-process FujiNet makes its runtime root the process's working
 * directory (its PC firmware opens data/ relative to it), so a path a user
 * gave relative to where they started the app -- a command-line argument --
 * is resolved against the directory the first session was created in.
 * paths_resolve returns `path` itself when it is absolute, else `buf`. */
void paths_capture_launch_dir(void);
const char *paths_resolve(const char *path, char *buf, size_t bufsz);
/* Locate libfujinet and provision the runtime tree (fnconfig.ini + data/ +
 * SD/) into <data>/fujinet on first run. Returns 0, or -1 if no runtime is
 * available (not fatal to the session -- see fujinet_start). */
int paths_provision_fujinet(struct studio2session *s);

void session_set_error(struct studio2session *s, const char *fmt, ...);

/* fujinet_runtime.c */
int  fujinet_start(struct studio2session *s);
void fujinet_stop(struct studio2session *s);
/* Block (up to timeout_ms) until the BoIP port accepts, so the cartridge's
 * first dial-out finds the listener. Returns 0 once up, -1 on timeout. */
int  fujinet_wait_for_boip(struct studio2session *s, int timeout_ms);

/* roms.c */
/* The BIOS as the console maps it ($0000-$07FF), assembled from what is
 * imported: the 2K set, or its ROMs (ic11 and ic12 at least); a malloc'd
 * buffer (caller frees) and its size, or NULL when there is none. */
uint8_t *roms_load_bios(struct studio2session *s, uint32_t *size);

/* audio_sdl.c */
int  audio_start(struct studio2session *s);
void audio_stop(struct studio2session *s);

/* gamepad_sdl.c */
int  gamepad_start(struct studio2session *s);
void gamepad_stop(struct studio2session *s);
/* Called by the gamepad thread with the state it resolved (input.c). A pad
 * has a stable owner number, 0..STUDIO2_MAX_PADS-1, while it is connected.
 * apply: the D-pad, hat or stick of `owner` holds keypad key `act` of `port`
 * (its STUDIO2_SRC_STICK source). target: a pad BUTTON holds or releases a
 * target (its STUDIO2_SRC_PAD source). release: the pad is gone or moved:
 * let go of everything it held. */
void session_gamepad_apply(struct studio2session *s, int owner, int port, int act, int down);
void session_gamepad_target(struct studio2session *s, int owner, int target, int down);
void session_gamepad_release(struct studio2session *s, int owner);
/* Called by the gamepad thread on a connect/disconnect. */
void session_gamepad_event(struct studio2session *s, const char *text);

/* debug.c: the session tells the debugger when the machine goes away and
 * comes back (a power cycle replaces it), and starts the FujiNet trace
 * before the first. */
void studio2debug_trace_fujinet(struct studio2session *s);
void studio2debug_before_power_cycle(struct studio2session *s);
void studio2debug_after_power_cycle(struct studio2session *s);
void studio2debug_destroy(struct studio2session *s);

#ifdef __cplusplus
}
#endif

#endif /* STUDIO2_SESSION_INTERNAL_H */
