; counter.asm -- the tests' cartridge image: counts frames of its own loop in
; RAM and reports keypad A's key 1, so a test can see it run and see input
; reach it. Entered as every Studio II client is ($0400's byte-code "04 02",
; native $0402 with P = 3), with its own display interrupt pointing the
; 1861's DMA at $F400 each frame. With COUNTER_APP it claims the FujiNet
; mailbox ("FUJI" at $07FC), so the stand-in runs it with no BIOS; without,
; it is a "game" that needs one.
;
;   $0810  loop counter (wraps)
;   $0811  1 while keypad A's key 1 is held, else 0
;   $0812  interrupts taken (wraps)
;
; Copyright (C) 2026 Thomas Cherryhomes
; SPDX-License-Identifier: GPL-3.0-or-later

        CPU     1802

R0      EQU     0
R1      EQU     1
R2      EQU     2
R3      EQU     3
R8      EQU     8
R9      EQU     9
RA      EQU     10

        ORG     0400H
        DB      04H, 02H
entry:  SEX     R3
        DIS                             ; X = 3, P = 3, IE = 0
        DB      33H
        LDI     isr >> 8
        PHI     R1
        LDI     isr & 0FFH
        PLO     R1
        LDI     09H                     ; stack at the top of RAM
        PHI     R2
        LDI     0FFH
        PLO     R2
        LDI     08H
        PHI     R8
        PHI     R9
        PHI     RA
        LDI     10H
        PLO     R8                      ; R8 -> $0810
        LDI     11H
        PLO     R9                      ; R9 -> $0811
        LDI     12H
        PLO     RA                      ; RA -> $0812
        LDI     0
        STR     R8
        STR     R9
        STR     RA
        LDI     0F4H
        PHI     R0
        LDI     0
        PLO     R0
        SEX     R3
        RET                             ; X = 2, P = 3, IE = 1
        DB      23H

loop:   LDN     R8
        ADI     1
        STR     R8
        SEX     R3
        OUT     2                       ; latch key 1 (the byte inline)
        DB      01H
        SEX     R2
        LDI     0
        BN3     up
        LDI     1
up:     STR     R9
        BR      loop

isrx:   LDA     R2                      ; D back
        RET                             ; leaves R1 at isr
isr:    DEC     R2
        SAV                             ; T
        DEC     R2
        STR     R2                      ; D
        LBR     isr1                    ; DMA lands on an instruction boundary
isr1:   LDI     0F4H
        PHI     R0
        LDI     0
        PLO     R0
        LDN     RA
        ADI     1
        STR     RA
isrw:   B1      isrw                    ; EF1 drops at the first DMA
        BR      isrx

        IFDEF   COUNTER_APP
        ORG     07FCH
        DB      "FUJI"
        ENDIF

        END
