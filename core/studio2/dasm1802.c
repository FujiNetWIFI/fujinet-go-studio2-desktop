/* dasm1802.c -- see dasm1802.h.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME; copyright-holders Curt Coder -- see COMPLIANCE.md)
 */

#include <stdio.h>

#include "dasm1802.h"

int dasm1802(uint16_t pc, dasm1802_peek_fn peek, void *user, int p_eq_x,
             char *buf, size_t bufsz, unsigned *flags_out, int32_t *target_out)
{
    const uint16_t base_pc = pc;
    const uint8_t opcode = peek(user, pc++);
    const int n = opcode & 0x0f;
    unsigned flags = 0;
    int32_t target = -1;

#define IMM()    (peek(user, pc++))
#define SHORT()  (target = (int32_t)((base_pc & 0xff00) | peek(user, pc)), pc++, (unsigned)target)
#define LONG()   (target = (int32_t)(peek(user, pc) << 8 | peek(user, (uint16_t)(pc + 1))), pc += 2, (unsigned)target)
#define SSKIP()  (target = (int32_t)(uint16_t)(pc + 1), (unsigned)target)
#define LSKIP()  (target = (int32_t)(uint16_t)(pc + 2), (unsigned)target)

    switch (opcode)
    {
    case 0x00: snprintf(buf, bufsz, "IDL"); break;
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
        snprintf(buf, bufsz, "LDN R%d", n); break;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        snprintf(buf, bufsz, "INC R%d", n); break;
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f:
        snprintf(buf, bufsz, "DEC R%d", n); break;
    case 0x30: snprintf(buf, bufsz, "BR %04X", SHORT()); break;
    case 0x31: snprintf(buf, bufsz, "BQ %04X", SHORT()); flags = DASM1802_STEP_COND; break;
    case 0x32: snprintf(buf, bufsz, "BZ %04X", SHORT()); flags = DASM1802_STEP_COND; break;
    case 0x33: snprintf(buf, bufsz, "BDF %04X", SHORT()); flags = DASM1802_STEP_COND; break;
    case 0x34: snprintf(buf, bufsz, "B1 %04X", SHORT()); flags = DASM1802_STEP_COND; break;
    case 0x35: snprintf(buf, bufsz, "B2 %04X", SHORT()); flags = DASM1802_STEP_COND; break;
    case 0x36: snprintf(buf, bufsz, "B3 %04X", SHORT()); flags = DASM1802_STEP_COND; break;
    case 0x37: snprintf(buf, bufsz, "B4 %04X", SHORT()); flags = DASM1802_STEP_COND; break;
    case 0x38: snprintf(buf, bufsz, "SKP %04X", SSKIP()); break;
    case 0x39: snprintf(buf, bufsz, "BNQ %04X", SHORT()); flags = DASM1802_STEP_COND; break;
    case 0x3a: snprintf(buf, bufsz, "BNZ %04X", SHORT()); flags = DASM1802_STEP_COND; break;
    case 0x3b: snprintf(buf, bufsz, "BNF %04X", SHORT()); flags = DASM1802_STEP_COND; break;
    case 0x3c: snprintf(buf, bufsz, "BN1 %04X", SHORT()); flags = DASM1802_STEP_COND; break;
    case 0x3d: snprintf(buf, bufsz, "BN2 %04X", SHORT()); flags = DASM1802_STEP_COND; break;
    case 0x3e: snprintf(buf, bufsz, "BN3 %04X", SHORT()); flags = DASM1802_STEP_COND; break;
    case 0x3f: snprintf(buf, bufsz, "BN4 %04X", SHORT()); flags = DASM1802_STEP_COND; break;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
        snprintf(buf, bufsz, "LDA R%d", n); break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
        snprintf(buf, bufsz, "STR R%d", n); break;
    case 0x60:
        if (p_eq_x)
            snprintf(buf, bufsz, "IRX; DB #%02X", IMM());
        else
            snprintf(buf, bufsz, "IRX");
        break;
    case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
        if (p_eq_x)
            snprintf(buf, bufsz, "OUT %d; DB #%02X", opcode & 7, IMM());
        else
            snprintf(buf, bufsz, "OUT %d", opcode & 7);
        break;
    case 0x68: snprintf(buf, bufsz, "illegal"); break;
    case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: case 0x6e: case 0x6f:
        snprintf(buf, bufsz, "INP %d", opcode & 7); break;
    case 0x70: snprintf(buf, bufsz, "RET"); flags = DASM1802_STEP_OUT; break;
    case 0x71: snprintf(buf, bufsz, "DIS"); flags = DASM1802_STEP_OUT; break;
    case 0x72: snprintf(buf, bufsz, "LDXA"); break;
    case 0x73: snprintf(buf, bufsz, "STXD"); break;
    case 0x74: snprintf(buf, bufsz, "ADC"); break;
    case 0x75: snprintf(buf, bufsz, "SDB"); break;
    case 0x76: snprintf(buf, bufsz, "SHRC"); break;
    case 0x77: snprintf(buf, bufsz, "SMB"); break;
    case 0x78: snprintf(buf, bufsz, "SAV"); break;
    case 0x79: snprintf(buf, bufsz, "MARK"); break;
    case 0x7a: snprintf(buf, bufsz, "REQ"); break;
    case 0x7b: snprintf(buf, bufsz, "SEQ"); break;
    case 0x7c: snprintf(buf, bufsz, "ADCI #%02X", IMM()); break;
    case 0x7d: snprintf(buf, bufsz, "SDBI #%02X", IMM()); break;
    case 0x7e: snprintf(buf, bufsz, "SHLC"); break;
    case 0x7f: snprintf(buf, bufsz, "SMBI #%02X", IMM()); break;
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8c: case 0x8d: case 0x8e: case 0x8f:
        snprintf(buf, bufsz, "GLO R%d", n); break;
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f:
        snprintf(buf, bufsz, "GHI R%d", n); break;
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: case 0xa6: case 0xa7:
    case 0xa8: case 0xa9: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
        snprintf(buf, bufsz, "PLO R%d", n); break;
    case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
    case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
        snprintf(buf, bufsz, "PHI R%d", n); break;
    case 0xc0: snprintf(buf, bufsz, "LBR %04X", LONG()); break;
    case 0xc1: snprintf(buf, bufsz, "LBQ %04X", LONG()); flags = DASM1802_STEP_COND; break;
    case 0xc2: snprintf(buf, bufsz, "LBZ %04X", LONG()); flags = DASM1802_STEP_COND; break;
    case 0xc3: snprintf(buf, bufsz, "LBDF %04X", LONG()); flags = DASM1802_STEP_COND; break;
    case 0xc4: snprintf(buf, bufsz, "NOP"); break;
    case 0xc5: snprintf(buf, bufsz, "LSNQ %04X", LSKIP()); flags = DASM1802_STEP_COND; break;
    case 0xc6: snprintf(buf, bufsz, "LSNZ %04X", LSKIP()); flags = DASM1802_STEP_COND; break;
    case 0xc7: snprintf(buf, bufsz, "LSNF %04X", LSKIP()); flags = DASM1802_STEP_COND; break;
    case 0xc8: snprintf(buf, bufsz, "LSKP %04X", LSKIP()); break;
    case 0xc9: snprintf(buf, bufsz, "LBNQ %04X", LONG()); flags = DASM1802_STEP_COND; break;
    case 0xca: snprintf(buf, bufsz, "LBNZ %04X", LONG()); flags = DASM1802_STEP_COND; break;
    case 0xcb: snprintf(buf, bufsz, "LBNF %04X", LONG()); flags = DASM1802_STEP_COND; break;
    case 0xcc: snprintf(buf, bufsz, "LSIE %04X", LSKIP()); flags = DASM1802_STEP_COND; break;
    case 0xcd: snprintf(buf, bufsz, "LSQ %04X", LSKIP()); flags = DASM1802_STEP_COND; break;
    case 0xce: snprintf(buf, bufsz, "LSZ %04X", LSKIP()); flags = DASM1802_STEP_COND; break;
    case 0xcf: snprintf(buf, bufsz, "LSDF %04X", LSKIP()); flags = DASM1802_STEP_COND; break;
    case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0xd4: case 0xd5: case 0xd6: case 0xd7:
    case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf:
        snprintf(buf, bufsz, "SEP R%d", n); flags = DASM1802_STEP_OVER; break;
    case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe4: case 0xe5: case 0xe6: case 0xe7:
    case 0xe8: case 0xe9: case 0xea: case 0xeb: case 0xec: case 0xed: case 0xee: case 0xef:
        snprintf(buf, bufsz, "SEX R%d", n); break;
    case 0xf0: snprintf(buf, bufsz, "LDX"); break;
    case 0xf1: snprintf(buf, bufsz, "OR"); break;
    case 0xf2: snprintf(buf, bufsz, "AND"); break;
    case 0xf3: snprintf(buf, bufsz, "XOR"); break;
    case 0xf4: snprintf(buf, bufsz, "ADD"); break;
    case 0xf5: snprintf(buf, bufsz, "SD"); break;
    case 0xf6: snprintf(buf, bufsz, "SHR"); break;
    case 0xf7: snprintf(buf, bufsz, "SM"); break;
    case 0xf8: snprintf(buf, bufsz, "LDI #%02X", IMM()); break;
    case 0xf9: snprintf(buf, bufsz, "ORI #%02X", IMM()); break;
    case 0xfa: snprintf(buf, bufsz, "ANI #%02X", IMM()); break;
    case 0xfb: snprintf(buf, bufsz, "XRI #%02X", IMM()); break;
    case 0xfc: snprintf(buf, bufsz, "ADI #%02X", IMM()); break;
    case 0xfd: snprintf(buf, bufsz, "SDI #%02X", IMM()); break;
    case 0xfe: snprintf(buf, bufsz, "SHL"); break;
    default:   snprintf(buf, bufsz, "SMI #%02X", IMM()); break;
    }

#undef IMM
#undef SHORT
#undef LONG
#undef SSKIP
#undef LSKIP

    if (flags_out)
        *flags_out = flags;
    if (target_out)
        *target_out = target;
    return (int)(uint16_t)(pc - base_pc);
}
