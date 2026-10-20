/* dasm1802.h -- the CDP1802 disassembler.
 *
 * Transposed from MAME's src/devices/cpu/cosmac/cosdasm.cpp (BSD-3-Clause,
 * copyright-holders Curt Coder), CDP1802 variant: the same mnemonics and
 * operand formats, so a listing here and MAME's debugger read alike
 * (core/tests/dasm_test.c holds the two to each other).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME; copyright-holders Curt Coder -- see COMPLIANCE.md)
 */

#ifndef STUDIO2_DASM1802_H
#define STUDIO2_DASM1802_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the instruction does to the flow (MAME's STEP_* flags). */
#define DASM1802_STEP_OVER 0x01     /* SEP: a subroutine call */
#define DASM1802_STEP_OUT  0x02     /* RET / DIS */
#define DASM1802_STEP_COND 0x04     /* a conditional branch or skip */

typedef uint8_t (*dasm1802_peek_fn)(void *user, uint16_t addr);

/* Disassemble the instruction at `pc` into buf. `p_eq_x` shows an OUT's
 * inline data byte (P == X, MAME's config hook). Returns the length in
 * bytes (1-3); *flags gets DASM1802_STEP_*, *target the branch or skip
 * target, or -1. */
int dasm1802(uint16_t pc, dasm1802_peek_fn peek, void *user, int p_eq_x,
             char *buf, size_t bufsz, unsigned *flags, int32_t *target);

#ifdef __cplusplus
}
#endif

#endif /* STUDIO2_DASM1802_H */
