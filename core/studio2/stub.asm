; stub.asm -- the no-BIOS stand-in, this app's own code (not RCA's).
;
; Served at $0000-$03FF when no Studio II BIOS has been imported. It boots a
; FujiNet app -- the baked CONFIG, or any image with "FUJI" at $07FC -- the
; way the BIOS reaches one: the app's runtime (s2call.inc) is entered at
; $0402 with P = 3, and installs its own display interrupt from there.
; Commercial games are BIOS byte-code and need the real BIOS; for anything
; without the claim the stand-in parks with the display off, and the app
; says "Import BIOS".
;
; Entered at $0000 with P = 0 and X = 0 (power-on, CLEAR) or with P = 3 and
; X = 2 (fn_handover's RET of $23, after the cart swapped the image in), so
; the first instructions only use R4. Short branches only: no 3-cycle
; opcodes, no IDL (see pico/studio2/README.md in fujinet-firmware).
;
; Assemble: tools/stub/build-stub.sh (Macroassembler AS); the bytes live in
; core/studio2/stub.c.
;
; Copyright (C) 2026 Thomas Cherryhomes
; SPDX-License-Identifier: GPL-3.0-or-later

        CPU     1802

R0      EQU     0
R2      EQU     2
R3      EQU     3
R4      EQU     4
R5      EQU     5
R6      EQU     6

        ORG     0000H
        LDI     go >> 8                 ; any P: on to P = 4
        PHI     R4
        LDI     go & 0FFH
        PLO     R4
        SEP     R4

go:     SEX     R4
        DIS                             ; X = 4, P = 4, IE = 0
        DB      44H
        LDI     0F4H                    ; after a hand-over the display is
        PHI     R0                      ; still on: DMA at the cart's blank
        LDI     0                       ; raster meanwhile
        PLO     R0

        LDI     09H                     ; clear the console RAM, $09FF down
        PHI     R2                      ; to $0800, as the BIOS leaves it
        LDI     0FFH
        PLO     R2
        SEX     R2
clr:    LDI     0
        STXD
        GHI     R2
        XRI     07H
        BNZ     clr
        LDI     09H                     ; R2: the top of RAM again
        PHI     R2
        LDI     0FFH
        PLO     R2

        LDI     07H                     ; a FujiNet app claims the mailbox
        PHI     R6                      ; with "FUJI" at $07FC
        LDI     0FCH
        PLO     R6
        SEX     R6
        LDXA
        XRI     'F'
        BNZ     park
        LDXA
        XRI     'U'
        BNZ     park
        LDXA
        XRI     'J'
        BNZ     park
        LDX
        XRI     'I'
        BNZ     park

        LDI     4                       ; about two frames, as the BIOS
        PLO     R5                      ; takes ~50 ms to reach $0400: four
sync1:  BN1     sync1                   ; EF1 pulses (lines 76-80, 204-208),
sync2:  B1      sync2                   ; and at the end of each DMA starts
        LDI     0F4H                    ; over at the raster's top
        PHI     R0
        LDI     0
        PLO     R0
        DEC     R5
        GLO     R5
        BNZ     sync1

        SEX     R2
        INP     1                       ; display on (X = 2: the byte read
                                        ; lands on the stack's top)
        LDI     04H                     ; the app's native entry, as the
        PHI     R3                      ; BIOS byte-code "04 02" calls it:
        LDI     02H                     ; P = 3 at $0402
        PLO     R3
        SEP     R3

park:   BR      park                    ; not a FujiNet app: no BIOS to run it
                                        ; (the machine blanks the picture and
                                        ; the session asks for the BIOS)

        END
