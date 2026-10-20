/* cpu1802.c -- the CDP1802 state machine (see cpu1802.h).
 *
 * Transposed from MAME's src/devices/cpu/cosmac/cosmac.cpp (BSD-3-Clause,
 * copyright-holders Curt Coder): execute_run, run_state and the CDP1802
 * opcode handlers, in MAME's order. The opcode table becomes a switch; the
 * callbacks MAME leaves unset on the Studio II (WAIT, DMA IN, state codes,
 * TPA/TPB) are not carried over.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME; copyright-holders Curt Coder -- see COMPLIANCE.md)
 */

#include <string.h>

#include "cpu1802.h"

#define R   c->r
#define P   c->p
#define X   c->x
#define D   c->d
#define B   c->b
#define T   c->t
#define N   c->n
#define I   c->i
#define DF  c->df
#define IE  c->ie
#define Q   c->q
#define EF  c->ef

static inline uint8_t ram_r(cdp1802_t *c, uint16_t a)
{
    return c->bus.read(c->bus.user, a, CDP1802_READ_DATA);
}

static inline uint8_t opcode_r(cdp1802_t *c, uint16_t a)
{
    return c->bus.read(c->bus.user, a, CDP1802_READ_OPERAND);
}

static inline void ram_w(cdp1802_t *c, uint16_t a, uint8_t d)
{
    c->bus.write(c->bus.user, a, d);
}

static inline void set_q_flag(cdp1802_t *c, int state)
{
    Q = (uint8_t)state;
    if (c->bus.q_write)
        c->bus.q_write(c->bus.user, Q);
}

static inline void put_low_reg(cdp1802_t *c, int reg, uint8_t data)
{
    R[reg] = (uint16_t)((R[reg] & 0xff00) | data);
}

static inline void put_high_reg(cdp1802_t *c, int reg, uint8_t data)
{
    R[reg] = (uint16_t)((R[reg] & 0x00ff) | data << 8);
}

void cdp1802_init(cdp1802_t *c, const cdp1802_bus_t *bus)
{
    memset(c, 0, sizeof *c);
    c->bus = *bus;
    c->state = CDP1802_STATE_1_INIT;
    c->mode = CDP1802_MODE_RESET;
    c->pmode = CDP1802_MODE_RUN;
    c->wait = true;
    c->clear = true;
}

int cdp1802_length(uint8_t op)
{
    switch (op >> 4)
    {
    case 0x3: return op == 0x38 ? 1 : 2;        /* short branches; SKP */
    case 0xC:
        /* long branches take two bytes; NOP and the long skips none */
        switch (op & 0x0f)
        {
        case 0x0: case 0x1: case 0x2: case 0x3:
        case 0x9: case 0xa: case 0xb:
            return 3;
        default:
            return 1;
        }
    case 0x7:
        return (op == 0x7C || op == 0x7D || op == 0x7F) ? 2 : 1;
    case 0xF:
        return (op >= 0xF8 && op != 0xFE) ? 2 : 1;
    default:
        return 1;
    }
}

/*-------------------------------------------------
    opcode handlers (cosmac.cpp's, CDP1802 only)
-------------------------------------------------*/

static void add(cdp1802_t *c, int left, int right)
{
    int result = left + right;
    D = (uint8_t)(result & 0xff);
    DF = result > 0xff;
}

static void add_with_carry(cdp1802_t *c, int left, int right)
{
    int result = left + right + DF;
    D = (uint8_t)(result & 0xff);
    DF = result > 0xff;
}

static void subtract(cdp1802_t *c, int left, int right)
{
    int result = left + (right ^ 0xff) + 1;
    D = (uint8_t)(result & 0xff);
    DF = result > 0xff;
}

static void subtract_with_borrow(cdp1802_t *c, int left, int right)
{
    int result = left + (right ^ 0xff) + DF;
    D = (uint8_t)(result & 0xff);
    DF = result > 0xff;
}

static void short_branch(cdp1802_t *c, int taken)
{
    if (taken)
        put_low_reg(c, P, opcode_r(c, R[P]));
    else
        R[P]++;
}

static void long_branch(cdp1802_t *c, int taken)
{
    if (taken)
    {
        if (c->state == CDP1802_STATE_1_EXECUTE)
            B = opcode_r(c, R[P]++);                    /* S1#1 */
        else
            R[P] = (uint16_t)((B << 8) | opcode_r(c, R[P])); /* S1#2 */
    }
    else
    {
        R[P]++;                                         /* S1#1, S1#2 */
    }
}

static void long_skip(cdp1802_t *c, int taken)
{
    if (taken)
        R[P]++;                                         /* S1#1, S1#2 */
}

static void return_from_interrupt(cdp1802_t *c, int ie)
{
    uint8_t data = ram_r(c, R[X]);
    R[X]++;
    P = data & 0xf;
    X = data >> 4;
    IE = (uint8_t)ie;
}

static void execute_op(cdp1802_t *c)
{
    const uint8_t op = c->op;

    switch (op >> 4)
    {
    case 0x0:
        if (op != 0x00)
            D = ram_r(c, R[N]);                         /* LDN */
        /* IDL: idle */
        return;
    case 0x1: R[N]++; return;                           /* INC */
    case 0x2: R[N]--; return;                           /* DEC */
    case 0x3:
        switch (op & 0x0f)
        {
        case 0x0: short_branch(c, 1); break;            /* BR */
        case 0x1: short_branch(c, Q); break;            /* BQ */
        case 0x2: short_branch(c, D == 0); break;       /* BZ */
        case 0x3: short_branch(c, DF); break;           /* BDF */
        case 0x4: case 0x5: case 0x6: case 0x7:
            short_branch(c, EF[N & 0x03]); break;       /* B1-B4 */
        case 0x8: short_branch(c, 0); break;            /* SKP (NBR) */
        case 0x9: short_branch(c, !Q); break;           /* BNQ */
        case 0xa: short_branch(c, D != 0); break;       /* BNZ */
        case 0xb: short_branch(c, !DF); break;          /* BNF */
        default:
            short_branch(c, !EF[N & 0x03]); break;      /* BN1-BN4 */
        }
        return;
    case 0x4: D = ram_r(c, R[N]); R[N]++; return;       /* LDA */
    case 0x5: ram_w(c, R[N], D); return;                /* STR */
    case 0x6:
        if (op == 0x60)
        {
            R[X]++;                                     /* IRX */
        }
        else if (op < 0x68)
        {
            uint8_t v = ram_r(c, R[X]);                 /* OUT */
            if (c->bus.io_write)
                c->bus.io_write(c->bus.user, N, v);
            R[X]++;
        }
        else
        {
            D = c->bus.io_read ? c->bus.io_read(c->bus.user, N & 0x07) : 0xff; /* INP */
            ram_w(c, R[X], D);
        }
        return;
    case 0x7:
        switch (op & 0x0f)
        {
        case 0x0: return_from_interrupt(c, 1); break;   /* RET */
        case 0x1: return_from_interrupt(c, 0); break;   /* DIS */
        case 0x2: D = ram_r(c, R[X]); R[X]++; break;    /* LDXA */
        case 0x3: ram_w(c, R[X], D); R[X]--; break;     /* STXD */
        case 0x4: add_with_carry(c, ram_r(c, R[X]), D); break; /* ADC */
        case 0x5: subtract_with_borrow(c, ram_r(c, R[X]), D); break; /* SDB */
        case 0x6: { int b = DF; DF = D & 1; D >>= 1; D |= (uint8_t)(b << 7); } break; /* SHRC */
        case 0x7: subtract_with_borrow(c, D, ram_r(c, R[X])); break; /* SMB */
        case 0x8: ram_w(c, R[X], T); break;             /* SAV */
        case 0x9:                                       /* MARK */
            T = (uint8_t)((X << 4) | P);
            ram_w(c, R[2], T);
            X = P;
            R[2]--;
            break;
        case 0xa: set_q_flag(c, 0); break;              /* REQ */
        case 0xb: set_q_flag(c, 1); break;              /* SEQ */
        case 0xc: add_with_carry(c, ram_r(c, R[P]), D); R[P]++; break; /* ADCI */
        case 0xd: subtract_with_borrow(c, ram_r(c, R[P]), D); R[P]++; break; /* SDBI */
        case 0xe: { int b = DF; DF = (D >> 7) & 1; D = (uint8_t)(D << 1); D |= (uint8_t)b; } break; /* SHLC */
        default: subtract_with_borrow(c, D, ram_r(c, R[P])); R[P]++; break; /* SMBI */
        }
        return;
    case 0x8: D = (uint8_t)(R[N] & 0xff); return;       /* GLO */
    case 0x9: D = (uint8_t)(R[N] >> 8); return;         /* GHI */
    case 0xa: put_low_reg(c, N, D); return;             /* PLO */
    case 0xb: put_high_reg(c, N, D); return;            /* PHI */
    case 0xc:
        switch (op & 0x0f)
        {
        case 0x0: long_branch(c, 1); break;             /* LBR */
        case 0x1: long_branch(c, Q); break;             /* LBQ */
        case 0x2: long_branch(c, D == 0); break;        /* LBZ */
        case 0x3: long_branch(c, DF); break;            /* LBDF */
        case 0x4: break;                                /* NOP */
        case 0x5: long_skip(c, !Q); break;              /* LSNQ */
        case 0x6: long_skip(c, D != 0); break;          /* LSNZ */
        case 0x7: long_skip(c, !DF); break;             /* LSNF */
        case 0x8: long_skip(c, 1); break;               /* LSKP (NLBR) */
        case 0x9: long_branch(c, !Q); break;            /* LBNQ */
        case 0xa: long_branch(c, D != 0); break;        /* LBNZ */
        case 0xb: long_branch(c, !DF); break;           /* LBNF */
        case 0xc: long_skip(c, IE); break;              /* LSIE */
        case 0xd: long_skip(c, Q); break;               /* LSQ */
        case 0xe: long_skip(c, D == 0); break;          /* LSZ */
        default: long_skip(c, DF); break;               /* LSDF */
        }
        return;
    case 0xd: P = N; return;                            /* SEP */
    case 0xe: X = N; return;                            /* SEX */
    default:
        switch (op & 0x0f)
        {
        case 0x0: D = ram_r(c, R[X]); break;            /* LDX */
        case 0x1: D = ram_r(c, R[X]) | D; break;        /* OR */
        case 0x2: D = ram_r(c, R[X]) & D; break;        /* AND */
        case 0x3: D = ram_r(c, R[X]) ^ D; break;        /* XOR */
        case 0x4: add(c, ram_r(c, R[X]), D); break;     /* ADD */
        case 0x5: subtract(c, ram_r(c, R[X]), D); break; /* SD */
        case 0x6: DF = D & 1; D >>= 1; break;           /* SHR */
        case 0x7: subtract(c, D, ram_r(c, R[X])); break; /* SM */
        case 0x8: D = ram_r(c, R[P]); R[P]++; break;    /* LDI */
        case 0x9: D = ram_r(c, R[P]) | D; R[P]++; break; /* ORI */
        case 0xa: D = ram_r(c, R[P]) & D; R[P]++; break; /* ANI */
        case 0xb: D = ram_r(c, R[P]) ^ D; R[P]++; break; /* XRI */
        case 0xc: add(c, ram_r(c, R[P]), D); R[P]++; break; /* ADI */
        case 0xd: subtract(c, ram_r(c, R[P]), D); R[P]++; break; /* SDI */
        case 0xe: DF = (D >> 7) & 1; D = (uint8_t)(D << 1); break; /* SHL */
        default: subtract(c, D, ram_r(c, R[P])); R[P]++; break; /* SMI */
        }
        return;
    }
}

/*-------------------------------------------------
    the states (cosmac.cpp's CORE EXECUTION)
-------------------------------------------------*/

static inline bool check_irq(cdp1802_t *c)
{
    return IE && c->irq;
}

static void sample_ef_lines(cdp1802_t *c)
{
    for (int i = 0; i < 4; i++)
        EF[i] = (uint8_t)(c->bus.ef_read ? c->bus.ef_read(c->bus.user, i) : 0);
}

static void fetch_instruction(cdp1802_t *c)
{
    uint16_t addr = R[P]++;
    c->op = c->bus.read(c->bus.user, addr, CDP1802_READ_FETCH);
    I = c->op >> 4 & 0x0f;
    N = c->op & 0x0f;
    c->instructions++;
    c->state = CDP1802_STATE_1_EXECUTE;
}

void cdp1802_reset_state(cdp1802_t *c)
{
    c->state = CDP1802_STATE_1_INIT;
    c->op = 0;
    I = 0;
    N = 0;
    set_q_flag(c, 0);
    IE = 1;
}

static void initialize(cdp1802_t *c)
{
    T = (uint8_t)((X << 4) | P);
    X = 0;
    P = 0;
    R[0] = 0;

    if (c->dmain)
        c->state = CDP1802_STATE_2_DMA_IN;
    else if (c->dmaout)
        c->state = CDP1802_STATE_2_DMA_OUT;
    else
        c->state = CDP1802_STATE_0_FETCH;
}

static void execute_instruction(cdp1802_t *c)
{
    execute_op(c);

    if (c->state == CDP1802_STATE_1_EXECUTE && (c->op >> 4) == 0xc) /* "long" opcodes */
        c->state = CDP1802_STATE_1_EXECUTE_2ND;
    else if (c->dmain)
        c->state = CDP1802_STATE_2_DMA_IN;
    else if (c->dmaout)
        c->state = CDP1802_STATE_2_DMA_OUT;
    else if (check_irq(c))
        c->state = CDP1802_STATE_3_INT;
    else if (c->op != 0) /* not idling */
        c->state = CDP1802_STATE_0_FETCH;
}

static void dma_input(cdp1802_t *c)
{
    /* nothing on the Studio II drives DMA IN; kept for the state machine */
    R[0]++;

    if (c->dmain)
        c->state = CDP1802_STATE_2_DMA_IN;
    else if (c->dmaout)
        c->state = CDP1802_STATE_2_DMA_OUT;
    else if (check_irq(c))
        c->state = CDP1802_STATE_3_INT;
    else if (c->mode == CDP1802_MODE_LOAD)
        c->state = CDP1802_STATE_1_EXECUTE;
    else
        c->state = CDP1802_STATE_0_FETCH;
}

static void dma_output(cdp1802_t *c)
{
    uint16_t addr = R[0]++;
    uint8_t data = c->bus.read(c->bus.user, addr, CDP1802_READ_DMA);
    if (c->bus.dma_write)
        c->bus.dma_write(c->bus.user, addr, data);

    if (c->dmain)
        c->state = CDP1802_STATE_2_DMA_IN;
    else if (c->dmaout)
        c->state = CDP1802_STATE_2_DMA_OUT;
    else if (check_irq(c))
        c->state = CDP1802_STATE_3_INT;
    else
        c->state = CDP1802_STATE_0_FETCH;
}

static void interrupt(cdp1802_t *c)
{
    T = (uint8_t)((X << 4) | P);
    X = 2;
    P = 1;
    IE = 0;

    if (c->dmain)
        c->state = CDP1802_STATE_2_DMA_IN;
    else if (c->dmaout)
        c->state = CDP1802_STATE_2_DMA_OUT;
    else
        c->state = CDP1802_STATE_0_FETCH;
}

static void run_state(cdp1802_t *c)
{
    switch (c->state)
    {
    case CDP1802_STATE_0_FETCH:
        c->op = 0;
        fetch_instruction(c);
        break;
    case CDP1802_STATE_1_INIT:
        initialize(c);
        break;
    case CDP1802_STATE_1_EXECUTE:
        sample_ef_lines(c);
        execute_instruction(c);
        break;
    case CDP1802_STATE_1_EXECUTE_2ND:
        execute_instruction(c);
        break;
    case CDP1802_STATE_2_DMA_IN:
        dma_input(c);
        break;
    case CDP1802_STATE_2_DMA_OUT:
        dma_output(c);
        break;
    case CDP1802_STATE_3_INT:
        interrupt(c);
        break;
    }
}

int cdp1802_step(cdp1802_t *c)
{
    int clocks = CDP1802_CLOCKS_STATE;

    /* sample_wait_clear */
    c->pmode = c->mode;
    c->mode = (c->clear ? 2 : 0) | (c->wait ? 1 : 0);

    switch (c->mode)
    {
    case CDP1802_MODE_LOAD:
        if (c->pmode == CDP1802_MODE_RESET)
        {
            c->pmode = CDP1802_MODE_LOAD;
            c->state = CDP1802_STATE_1_INIT;
            run_state(c);
            c->state = CDP1802_STATE_1_EXECUTE;
        }
        else
        {
            c->op = 0;
            I = 0;
            N = 0;
            run_state(c);
        }
        break;

    case CDP1802_MODE_RESET:
        cdp1802_reset_state(c);
        clocks = 1;
        break;

    case CDP1802_MODE_PAUSE:
        clocks = 1;
        break;

    case CDP1802_MODE_RUN:
        switch (c->pmode)
        {
        case CDP1802_MODE_LOAD:
            /* RUN mode cannot be initiated from LOAD mode */
            c->mode = CDP1802_MODE_LOAD;
            clocks = 0;
            break;
        case CDP1802_MODE_RESET:
            c->pmode = CDP1802_MODE_RUN;
            c->state = CDP1802_STATE_1_INIT;
            run_state(c);
            break;
        case CDP1802_MODE_PAUSE:
            c->pmode = CDP1802_MODE_RUN;
            c->state = CDP1802_STATE_0_FETCH;
            run_state(c);
            break;
        default:
            run_state(c);
            break;
        }
        break;
    }

    c->at_fetch = c->state == CDP1802_STATE_0_FETCH && c->mode == CDP1802_MODE_RUN;
    if (c->at_fetch)
        c->pc = R[P];
    return clocks;
}
