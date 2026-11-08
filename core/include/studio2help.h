/*
 * studio2help -- the app's Help: data/help/help.md, built in by
 * tools/help/make-help.py as topics of blocks of styled spans, which every
 * frontend lays out with its own toolkit.
 *
 * A frontend shows the blocks whose platforms include its own bit. Ids are
 * GitHub's heading slugs ("controllers-and-keys"); a Help button names a
 * context instead ("prefs/Machine", "debugger/cpu"), which stays put when a
 * heading is renamed. Everything here is constant data and reentrant.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef STUDIO2HELP_H
#define STUDIO2HELP_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    STUDIO2HELP_GNOME = 1, STUDIO2HELP_KDE = 2, STUDIO2HELP_WINDOWS = 4, STUDIO2HELP_MACOS = 8
};

typedef enum {
    STUDIO2HELP_H2 = 0, STUDIO2HELP_H3,
    STUDIO2HELP_PARA,
    STUDIO2HELP_BULLET, STUDIO2HELP_BULLET2,    /* "- " and "  - " */
    STUDIO2HELP_NUMBERED,                     /* a step; number is its own */
    STUDIO2HELP_NOTE,
    STUDIO2HELP_PRE,                          /* one TEXT span, lines as they are */
    STUDIO2HELP_TABLE_HEAD,                   /* starts a table; its rows follow */
    STUDIO2HELP_TABLE_ROW
} studio2help_block_kind;

typedef enum {
    STUDIO2HELP_TEXT = 0, STUDIO2HELP_BOLD, STUDIO2HELP_ITALIC, STUDIO2HELP_CODE,
    STUDIO2HELP_KBD,                          /* keys; mac_text is the Mac's */
    STUDIO2HELP_LINK,                         /* target "#slug" or a URL */
    STUDIO2HELP_CELL                          /* tables: the next column starts */
} studio2help_span_kind;

typedef struct {
    int kind;
    const char *text;                       /* UTF-8 */
    const char *mac_text;                   /* KBD on macOS, or NULL: text */
    const char *target;
} studio2help_span;

typedef struct {
    int kind;
    unsigned platforms;                     /* STUDIO2HELP_* bits */
    int number;
    const char *id;                         /* headings: the slug */
    const char *context;                    /* headings: or NULL */
    const studio2help_span *spans;            /* headings: one TEXT span */
    int nspans;
    /* Tables: the width of each column but the last, in ems, for frontends
     * that set tab stops (the last column wraps under itself). */
    const unsigned char *col_em;
    int ncol_em;
} studio2help_block;

typedef struct {
    const char *id;
    const char *title;
    const char *context;                    /* or NULL */
    const studio2help_block *blocks;
    int nblocks;
} studio2help_topic;

int studio2help_topic_count(void);
const studio2help_topic *studio2help_topic_at(int t);

/* "slug", "#slug" or a context: its topic, and the heading's block (-1 for
 * the topic itself). 0, or -1 if there is none. */
int studio2help_find(const char *id, int *topic, int *block);

/* The span's text as platform shows it. */
const char *studio2help_span_text(const studio2help_span *sp, unsigned platform);

/* A heading block's text. */
const char *studio2help_heading(const studio2help_block *b);

/* The block after the section headed by block (the next heading at its
 * level or above, or the topic's end); -1: the whole topic. */
int studio2help_section_end(int topic, int block);

/* The section headed by block (-1: from the topic's title) as text on
 * platform: a line per block, cells separated by " | ". Returns its length
 * (truncated to dstsz - 1). */
int studio2help_plain_text(unsigned platform, int topic, int block, char *dst, int dstsz);

/* Sections that have every word of query (case-insensitive), best first:
 * where it is in a title, then how often. block -1 is a topic's start, up
 * to its first heading. Returns how many were written. */
typedef struct {
    int topic, block, score;
} studio2help_hit;
int studio2help_search(unsigned platform, const char *query, studio2help_hit *out, int max);

/* The first match of a word of query in text at byte from or after: where
 * it starts and its length, for highlighting. 1, or 0 if there is none. */
int studio2help_match(const char *text, const char *query, int from, int *start, int *len);

#ifdef __cplusplus
}
#endif

#endif /* STUDIO2HELP_H */
