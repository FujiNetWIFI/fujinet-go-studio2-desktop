/*
 * debugger_internal -- the engine's side of the session glue and the
 * prompt's view of the engine. Not installed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STUDIO2_DEBUGGER_INTERNAL_H
#define STUDIO2_DEBUGGER_INTERNAL_H

#include "studio2debug.h"
#include "symbols.h"

#ifdef __cplusplus
extern "C" {
#endif

studio2debug *studio2debug_create(void);
void studio2debug_free(studio2debug *d);
/* The machine is about to go away (a power cycle or a stop): resume and
 * unhook, remembering whether a window held the engine. */
void studio2debug_power_down(studio2debug *d);
/* A new machine is up: hook it again if a window held the engine (which
 * stops it at its first instruction, so a power cycle can be debugged from
 * the reset vector). */
void studio2debug_power_up(studio2debug *d);
/* The opened cartridge, for symbols next to it ("" for CONFIG). */
void studio2debug_set_cart_path(studio2debug *d, const char *path);
/* Feed the FujiNet trace from the transport, from now on. */
void studio2debug_fujinet_install(studio2debug *d);

/* For the prompt (prompt.c). */
symtab *studio2debug_symbols(studio2debug *d);
/* Evaluate an expression in the machine's context; 0 or -1 with err. */
int studio2debug_eval(studio2debug *d, const char *text, long *out, char *err, int errsz);
/* The 16K ROM bank mapped at addr now, or SYM_BANK_ANY. */
int studio2debug_bank_at(studio2debug *d, uint16_t addr);
/* Register names the evaluator knows, for completion. */
extern const char *const studio2debug_reg_names[];

#ifdef __cplusplus
}
#endif

#endif /* STUDIO2_DEBUGGER_INTERNAL_H */
