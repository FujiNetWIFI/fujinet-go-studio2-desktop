/*
 * dasm_test -- the CDP1802 disassembler against MAME's own (dasm_golden.h,
 * from MAME's debugger): every opcode's text and length, the branch and
 * skip targets, the step flags, and an OUT's inline byte when P == X.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu1802.h"
#include "dasm1802.h"
#include "dasm_golden.h"

static uint8_t mem[0x10000];
static int failures;

static void check(int ok, const char *what)
{
    if (!ok)
    {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static uint8_t peek(void *user, uint16_t a)
{
    (void)user;
    return mem[a];
}

int main(void)
{
    char buf[64], what[160];
    unsigned flags;
    int32_t target;

    for (int op = 0; op < 256; op++)
    {
        const uint16_t pc = (uint16_t)(0x400 + op * 3);
        mem[pc] = (uint8_t)op;
        mem[pc + 1] = 0x12;
        mem[pc + 2] = 0x34;
    }
    for (int op = 0; op < 256; op++)
    {
        const uint16_t pc = (uint16_t)(0x400 + op * 3);
        int len = dasm1802(pc, peek, NULL, 0, buf, sizeof buf, &flags, &target);
        snprintf(what, sizeof what, "%02X: \"%s\" (%d) vs MAME's \"%s\" (%d)", op, buf, len,
                 dasm_golden[op].text, dasm_golden[op].len);
        check(len == dasm_golden[op].len && strcmp(buf, dasm_golden[op].text) == 0, what);
        /* the CPU's own idea of the length agrees, bar OUT's inline byte */
        snprintf(what, sizeof what, "%02X: cdp1802_length %d vs %d", op, cdp1802_length((uint8_t)op), len);
        check(cdp1802_length((uint8_t)op) == len, what);
    }

    /* targets and flags */
    mem[0x0510] = 0x3A; mem[0x0511] = 0x80;            /* BNZ 0580 */
    dasm1802(0x0510, peek, NULL, 0, buf, sizeof buf, &flags, &target);
    check(target == 0x0580 && (flags & DASM1802_STEP_COND), "a short branch's target is in its page");
    mem[0x0600] = 0xC0; mem[0x0601] = 0xE4; mem[0x0602] = 0x00; /* LBR E400 */
    dasm1802(0x0600, peek, NULL, 0, buf, sizeof buf, &flags, &target);
    check(target == 0xE400 && flags == 0, "a long branch's target is its operand");
    mem[0x0610] = 0xCE;                                 /* LSZ */
    dasm1802(0x0610, peek, NULL, 0, buf, sizeof buf, &flags, &target);
    check(target == 0x0613, "a long skip's target is two bytes past it");
    mem[0x0620] = 0xD4;
    dasm1802(0x0620, peek, NULL, 0, buf, sizeof buf, &flags, &target);
    check(flags == DASM1802_STEP_OVER && target == -1, "SEP steps over");
    mem[0x0630] = 0x70;
    dasm1802(0x0630, peek, NULL, 0, buf, sizeof buf, &flags, &target);
    check(flags == DASM1802_STEP_OUT, "RET steps out");

    /* OUT with P == X carries its data inline (MAME's config hook) */
    mem[0x0640] = 0x62; mem[0x0641] = 0x05;
    check(dasm1802(0x0640, peek, NULL, 1, buf, sizeof buf, &flags, &target) == 2
          && strcmp(buf, "OUT 2; DB #05") == 0, "OUT 2 with P == X shows its inline byte");
    mem[0x0650] = 0x60; mem[0x0651] = 0x07;
    check(dasm1802(0x0650, peek, NULL, 1, buf, sizeof buf, &flags, &target) == 2
          && strcmp(buf, "IRX; DB #07") == 0, "IRX with P == X too, as MAME prints it");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
