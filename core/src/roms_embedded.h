/*
 * roms_embedded -- the table tools/roms/embed-roms.py generates into the
 * build tree: empty (the shipping configuration, WITH_STUDIO2_ROMS=OFF) or the
 * recognised images a developer put in tools/roms/ (NOT redistributable;
 * see COMPLIANCE.md).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STUDIO2_ROMS_EMBEDDED_H
#define STUDIO2_ROMS_EMBEDDED_H

#include <stdint.h>

typedef struct {
    const char *file;          /* MAME's file name: "84932.ic11" */
    const uint8_t *data;
    uint32_t size;
} studio2_embedded_rom;

extern const studio2_embedded_rom studio2_embedded_roms[];
extern const unsigned studio2_embedded_rom_count;

#endif /* STUDIO2_ROMS_EMBEDDED_H */
