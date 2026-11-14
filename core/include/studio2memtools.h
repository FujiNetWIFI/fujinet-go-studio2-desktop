/*
 * Watches, range parsing, memory-search queries and breakpoint descriptions,
 * kept in the core so the debugger windows only lay them out.
 *
 * A watch is text the user typed (an address, a label, an expression, or a
 * range lo-hi / lo..hi) shown with its live value. It is stored as text and
 * resolved every time it is shown, so loading symbols later makes a label
 * start resolving.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef STUDIO2_MEMTOOLS_H
#define STUDIO2_MEMTOOLS_H

#include <stdint.h>

#include "studio2debug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "lo..hi", "lo-hi" (plain numbers or labels on both sides), or one
 * expression (an address, label, label+3, [ptr]). A bare token of 1-4 hex
 * digits ("0810") is hex unless it names a label; "$0810" and expressions
 * work as in the console. Returns 1 and fills lo and hi (inclusive,
 * lo <= hi), 0 if the text means nothing. */
int studio2sym_parse_range(studio2debug *d, const char *text, uint16_t *lo,
                       uint16_t *hi);

/* ---- watches ---- */
#define STUDIO2WATCH_MAX 64
/* The process-wide list, like the symbol tables: it outlives the window. */
int  studio2watch_count(void);
/* Returns the new index, or -1 if empty / full. A watch that does not resolve
 * yet is kept (it shows "?"). */
int  studio2watch_add(const char *text);
void studio2watch_remove(int index);
void studio2watch_clear(void);
const char *studio2watch_text(int index);
unsigned studio2watch_generation(void);
/* "score  $0810  $42 (66)". While the machine runs the value is "-"; a range
 * shows its first 8 bytes. Returns the length. */
int  studio2watch_format(studio2debug *d, int index, char *dst, int dstsz);
/* The range a watch covers (for "show in Memory"). 1 if it resolves. */
int  studio2watch_range(studio2debug *d, int index, uint16_t *lo, uint16_t *hi);

/* ---- memory search ---- */
enum { STUDIO2SEARCH_HEX = 0, STUDIO2SEARCH_TEXT };
#define STUDIO2SEARCH_MAX 32
/* Hex: byte pairs, optional "$" / "0x" / spaces. Text: one byte per
 * character, case-insensitive. Fills pat, n, mask (0xFF) and flags for
 * studio2debug_search. Returns 1, or 0 for empty / malformed text. */
int studio2search_parse(int kind, const char *text, uint8_t *pat, int *n,
                    uint8_t *mask, int *flags);

/* ---- breakpoints ---- */
/* "Write  $0810 (score)  if value != old"; IN / OUT breakpoints show the
 * port number ("In/Out  2"). */
int studio2bp_describe(studio2debug *d, const studio2debug_breakpoint *b, char *dst,
                   int dstsz);

/* ---- memory map ---- */
/* The name of the region of `map` (from studio2debug_memory_map) holding addr
 * ("Console RAM", "Cartridge bank window (bank 0/3)"), for labelling Memory rows. Fetch
 * the map once per refresh and look rows up in it. Returns the length, 0 and
 * "" when no region covers addr. A watch's range for "Show in Memory" is
 * studio2watch_range above. */
int studio2mem_region_name(const studio2debug_region *map, int count, uint16_t addr,
                       char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* STUDIO2_MEMTOOLS_H */
