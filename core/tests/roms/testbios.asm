; testbios.asm -- a "BIOS" of the tests' own: what the real RCA BIOS does for
; a cartridge that matters here, and nothing else -- turn the display on and
; enter the cartridge at native $0402 with P = 3 -- so the tests exercise
; Import BIOS and a game's boot with no RCA firmware anywhere. 2K, so it
; imports as the set (with a warning: MAME does not know its CRC).
;
; Copyright (C) 2026 Thomas Cherryhomes
; SPDX-License-Identifier: GPL-3.0-or-later

        CPU     1802

R0      EQU     0
R2      EQU     2
R3      EQU     3
R4      EQU     4

        ORG     0000H
        LDI     go >> 8                 ; any P: on to P = 4
        PHI     R4
        LDI     go & 0FFH
        PLO     R4
        SEP     R4
go:     SEX     R4
        DIS                             ; X = 4, P = 4, IE = 0
        DB      44H
        LDI     09H
        PHI     R2
        LDI     0FFH
        PLO     R2
        LDI     0F4H
        PHI     R0
        LDI     0
        PLO     R0
        SEX     R2
        INP     1                       ; display on
        LDI     04H
        PHI     R3
        LDI     02H
        PLO     R3
        SEP     R3

        ORG     07FFH                   ; 2K, as the set is
        DB      0

        END
