/* studio2_internal.h -- the emulated machine, shared by the core modules.
 *
 * The machine logic is transposed from MAME's RCA Studio II driver
 * (BSD-3-Clause, copyright-holders Curt Coder; see COMPLIANCE.md):
 *
 *   src/mame/rca/studio2.cpp            the memory and I/O maps, the keypads
 *                                       on EF3/EF4 through the OUT 2 latch,
 *                                       INP 1 (display on), Q to the beeper,
 *                                       the Clear key
 *   src/devices/cpu/cosmac/cosmac.cpp   the CDP1802 (cpu1802.c)
 *   src/devices/video/cdp1861.cpp       the CDP1861 (cdp1861.c)
 *   src/devices/sound/beep.cpp          the 300 Hz beeper
 *
 * Field names keep MAME's own (minus the m_ prefix) where there is one. The
 * Visicom and the MPT-02 family are not carried over: this app's machine is
 * the RCA Studio II, with the FujiNet cartridge always in the slot.
 *
 * MAME runs the CPU and the 1861's timers in its scheduler; here the
 * machine runs the CPU one state at a time and fires every 1861 timer due
 * at or before each state's start first, which is exactly what MAME's
 * timeslicing amounts to with both at 1.76 MHz (cpu1802.h, cdp1861.h). The
 * beam's origin -- line 0, the start of VBLANK -- is power-on, as in MAME.
 */

#ifndef STUDIO2_INTERNAL_H
#define STUDIO2_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu1802.h"
#include "cdp1861.h"
#include "fujinet_cart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- clocks (studio2.cpp: "the real clock is derived from an oscillator
 * circuit"; MAME uses 1.76 MHz for the CPU and the 1861 alike) ---- */
#define STUDIO2_CPU_CLOCK      1760000
#define STUDIO2_FRAME_CLOCKS   CDP1861_FRAME_CLOCKS           /* 29344 */
#define STUDIO2_FRAME_HZ       ((double)STUDIO2_CPU_CLOCK / STUDIO2_FRAME_CLOCKS) /* 59.98 */

/* ---- the picture: MAME's visible area, 100 x 246 ---- */
#define STUDIO2_FB_WIDTH       CDP1861_VIS_W
#define STUDIO2_FB_HEIGHT      CDP1861_VIS_H

/* ---- memory (studio2_map, machine_start) ---- */
#define STUDIO2_RAM_SIZE       0x200        /* mirrored on every A9 = 0 page */
#define STUDIO2_BIOS_SIZE      0x800        /* ic11-ic14: the system ROM, then
                                               the built-in games at $0400 */
#define STUDIO2_ROM_SERVED     0x400        /* the cart always claims $0400-$07FF */

/* ---- the no-BIOS stand-in (stub.asm) ---- */
#define STUDIO2_STUB_SIZE      92
#define STUDIO2_STUB_PARK      0x005A       /* "not a FujiNet app" */
extern const uint8_t studio2_stub[STUDIO2_STUB_SIZE];

/* ---- sound ---- */
#define STUDIO2_AUDIO_RATE     48000
#define STUDIO2_BEEP_HZ        300          /* BEEP(config, m_beeper, 300) */
#define STUDIO2_AUDIO_MAX      2048         /* stereo frames buffered per run */

/* ---- the keypads: bit n is key n (0-9), active high ---- */
#define STUDIO2_KEYPAD_A       0
#define STUDIO2_KEYPAD_B       1
#define STUDIO2_KEYS_MASK      0x03ff

typedef struct studio2_machine studio2_machine_t;

struct studio2_machine {
    cdp1802_t cpu;
    cdp1861_t vdc;
    uint64_t cycles;              /* clocks since power-on */
    uint64_t frame_end;           /* the next VBLANK (line 0) */
    uint64_t frame_count;

    /* memory */
    uint8_t ram[STUDIO2_RAM_SIZE];
    uint8_t rom[STUDIO2_BIOS_SIZE]; /* the BIOS, or the stand-in at $0000 */
    bool have_bios;
    bool stub_parked;             /* the stand-in met a game it cannot run */

    /* I/O (studio2_state) */
    uint8_t keylatch;
    uint16_t keys_live[2], keys[2]; /* live = set by any thread; keys = latched */
    bool clear_live, clear_held;  /* the Clear key (the CPU's CLEAR line) */
    uint8_t last_out[8];          /* the last byte OUT to each port */
    uint32_t inp1_count;          /* display-on strobes since power-on */

    studio2_cart_t *cart;         /* the FujiNet cartridge, or NULL (tests) */

    /* the beeper (beep.cpp), streamed at STUDIO2_AUDIO_RATE */
    bool beep_enable;
    int32_t beep_incr;
    float beep_signal;
    float beep_gain;
    uint64_t audio_samples;       /* samples produced since power-on */
    float out[2 * STUDIO2_AUDIO_MAX]; /* interleaved stereo */
    int out_frames;

    /* the picture, as MAME's screen_update leaves it at each VBLANK */
    uint32_t fb[STUDIO2_FB_WIDTH * STUDIO2_FB_HEIGHT];

    /* ---- debugger ----
     * instr_hook fires at every instruction boundary (the next state is a
     * fetch), on the emulation thread, BEFORE the fetch; it may block (that
     * is how the debugger pauses the machine). bus_hook fires on memory and
     * I/O accesses, only while `watch` is nonzero. */
    void (*instr_hook)(studio2_machine_t *m, void *user);
    void *instr_hook_user;
    void (*bus_hook)(studio2_machine_t *m, void *user, int kind, uint16_t addr,
                     uint8_t data);
    void *bus_hook_user;
    uint8_t watch;                /* STUDIO2_WATCH_* */
};

#define STUDIO2_WATCH_MEMR 0x01
#define STUDIO2_WATCH_MEMW 0x02
#define STUDIO2_WATCH_IOR  0x04
#define STUDIO2_WATCH_IOW  0x08
#define STUDIO2_WATCH_DMA  0x10

#define STUDIO2_BUS_MEMR 1
#define STUDIO2_BUS_MEMW 2
#define STUDIO2_BUS_IOR  4
#define STUDIO2_BUS_IOW  8
#define STUDIO2_BUS_DMA  16

/* machine.c */
/* Power on. `bios` (bios_size bytes: the 2K set or at least the 1K system
 * ROM), or NULL for the stand-in. The cart is attached, not owned, and
 * powered on by the caller. `mame_power_on` reproduces MAME's quirk that
 * the CPU never runs its CLEAR reset at power-on (IE stays 0); otherwise
 * power-on pulses CLEAR as the console's reset circuit does. */
void studio2_machine_init(studio2_machine_t *m, const uint8_t *bios, uint32_t bios_size,
                          studio2_cart_t *cart, bool mame_power_on);
/* Run to the next VBLANK; m->fb holds the frame, m->out the audio. */
void studio2_machine_run_frame(studio2_machine_t *m);
/* One pass of the CPU (one state, or one clock under CLEAR), hooks and all.
 * Returns its clocks. */
int studio2_machine_step(studio2_machine_t *m);
/* Run to the next instruction boundary (debugger). */
void studio2_machine_step_instruction(studio2_machine_t *m);
/* One pass with no debugger hooks (the debugger's own stepping). */
int studio2_machine_step_quiet(studio2_machine_t *m);
/* Latch the live inputs (the frame boundary): keypads, and the Clear key --
 * pressing it resets the 1861 (machine_reset) and holds the CPU in RESET,
 * releasing it lets the CPU run from $0000. */
void studio2_machine_latch_inputs(studio2_machine_t *m);
/* The picture as it stands now (the debugger shows a frame being drawn). */
void studio2_machine_snapshot(studio2_machine_t *m, uint32_t *dst);
/* Debugger access: peek never disturbs the machine (no hotspot fires, the
 * raster reads 0); poke writes console RAM, or the cart's own buffers. */
uint8_t studio2_machine_peek(studio2_machine_t *m, uint16_t addr);
void studio2_machine_poke(studio2_machine_t *m, uint16_t addr, uint8_t data);
/* What the console itself serves where the cart does not drive. */
uint8_t studio2_machine_console_read(const studio2_machine_t *m, uint16_t addr);

#ifdef __cplusplus
}
#endif

#endif /* STUDIO2_INTERNAL_H */
