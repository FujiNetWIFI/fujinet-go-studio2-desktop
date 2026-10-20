/* cpu1802.h -- the RCA CDP1802 COSMAC, the Studio II's CPU.
 *
 * Transposed from MAME's COSMAC core (src/devices/cpu/cosmac/cosmac.cpp and
 * cosmac.h, BSD-3-Clause, copyright-holders Curt Coder): the CDP1802 subset
 * only (no 1801 opcode map, no 1804/5/6 counter or extended opcodes). Field
 * and state names keep MAME's own, minus the m_ prefix, so the two can be
 * diffed side by side.
 *
 * MAME runs the state machine inside its scheduler: every state costs 8
 * clocks, and a state that starts at clock s sees every input line as the
 * timers left it at or before s. cdp1802_step() is one pass of MAME's
 * execute_run loop -- one state, or one clock while CLEAR holds the CPU in
 * RESET -- and the machine (machine.c) sets the lines before each pass.
 *
 * Every memory read goes through `read` with its kind (fetch, operand or
 * data, DMA), because on the Studio II every read crosses the cartridge
 * edge and the FujiNet cart decides each one by address and time.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME; copyright-holders Curt Coder -- see COMPLIANCE.md)
 */

#ifndef STUDIO2_CPU1802_H
#define STUDIO2_CPU1802_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* cosmac_mode: (CLEAR << 1) | WAIT */
enum {
    CDP1802_MODE_LOAD = 0,
    CDP1802_MODE_RESET,
    CDP1802_MODE_PAUSE,
    CDP1802_MODE_RUN,
};

/* cosmac_state */
enum {
    CDP1802_STATE_0_FETCH = 0,
    CDP1802_STATE_1_INIT,
    CDP1802_STATE_1_EXECUTE,
    CDP1802_STATE_1_EXECUTE_2ND,
    CDP1802_STATE_2_DMA_IN,
    CDP1802_STATE_2_DMA_OUT,
    CDP1802_STATE_3_INT,
};

/* What a read is for. */
enum {
    CDP1802_READ_FETCH = 0,     /* the opcode, S0 */
    CDP1802_READ_OPERAND,       /* an immediate or branch byte, at R(P) */
    CDP1802_READ_DATA,          /* M(R(N)) / M(R(X)) */
    CDP1802_READ_DMA,           /* DMA OUT, at R0 */
};

#define CDP1802_CLOCKS_STATE 8  /* fetch, execute, DMA and interrupt; MAME
                                   uses 8 for INIT too ("really 9") */

typedef struct cdp1802 cdp1802_t;

typedef struct {
    void *user;
    uint8_t (*read)(void *user, uint16_t addr, int kind);
    void (*write)(void *user, uint16_t addr, uint8_t data);
    uint8_t (*io_read)(void *user, int port);       /* INP 1-7 */
    void (*io_write)(void *user, int port, uint8_t data); /* OUT 1-7 */
    int (*ef_read)(void *user, int n);             /* EF1-4 as n = 0..3; 1 asserted */
    void (*q_write)(void *user, int q);
    void (*dma_write)(void *user, uint16_t addr, uint8_t data); /* DMA OUT */
} cdp1802_bus_t;

struct cdp1802 {
    cdp1802_bus_t bus;

    /* registers */
    uint16_t r[16];
    uint8_t p, x, d, b, t;
    uint8_t n, i;
    uint8_t df, ie, q;
    uint8_t ef[4];          /* sampled at the start of S1 */
    uint8_t op;

    /* control */
    int state;
    int mode, pmode;
    bool wait, clear;       /* the lines; 1 = not asserted (active low) */
    int irq;                /* INTERRUPT, 1 asserted */
    int dmain, dmaout;      /* DMA IN / DMA OUT, 1 asserted */

    /* the debugger's view */
    uint16_t pc;            /* R(P) of the instruction being fetched */
    bool at_fetch;          /* the next pass is S0: an instruction boundary */
    uint64_t instructions;
};

/* MAME's constructor + device_start: everything 0, state INIT, mode RESET,
 * previous mode RUN, so the first pass with CLEAR high runs INIT WITHOUT
 * reset_state() -- IE stays 0, which is MAME's power-on (see machine.c). */
void cdp1802_init(cdp1802_t *c, const cdp1802_bus_t *bus);

/* reset_state(): what holding CLEAR low does each clock (Q off, IE on). */
void cdp1802_reset_state(cdp1802_t *c);

/* One pass of execute_run: returns the clocks it took (8 for a state, 1 for
 * a RESET or PAUSE clock). The machine updates the lines before calling. */
int cdp1802_step(cdp1802_t *c);

/* For the disassembler and the debugger: the instruction length at an
 * opcode (1, 2 or 3). */
int cdp1802_length(uint8_t op);

#ifdef __cplusplus
}
#endif

#endif /* STUDIO2_CPU1802_H */
