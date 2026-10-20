/* machine.c -- the RCA Studio II (see studio2_internal.h).
 *
 * Transposed from MAME's src/mame/rca/studio2.cpp (BSD-3-Clause,
 * copyright-holders Curt Coder): studio2_map / studio2_io_map,
 * machine_start's ROM and cart handlers, dispon_r, keylatch_w, clear_r,
 * ef3_r, ef4_r, q_w, reset_w, plus MAME's beep_device for the sound.
 *
 * With the FujiNet cartridge every read crosses the edge first (as the
 * firmware's MAME device installs one read handler over the whole space);
 * what the cart does not drive is the console's own: the BIOS (or the
 * stand-in) below $0400, the 512 bytes of RAM on every A9 = 0 page from
 * $0800 up, open bus elsewhere. Writes never reach the cart -- the edge has
 * no write strobe.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME; copyright-holders Curt Coder -- see COMPLIANCE.md)
 */

#include <string.h>

#include "studio2_internal.h"

/*-------------------------------------------------
    the bus
-------------------------------------------------*/

uint8_t studio2_machine_console_read(const studio2_machine_t *m, uint16_t a)
{
    if (a < STUDIO2_ROM_SERVED)
        return m->rom[a];
    if (!m->cart && a < STUDIO2_BIOS_SIZE)
        return m->rom[a];                     /* no cart: the built-in games */
    if (a >= 0x0800 && !(a & 0x0200))
        return m->ram[a & (STUDIO2_RAM_SIZE - 1)];
    return 0xff;
}

static uint8_t bus_read(void *user, uint16_t a, int kind)
{
    studio2_machine_t *m = user;
    uint8_t d;

    if (!m->cart || !studio2_cart_read(m->cart, a,
                                       (uint32_t)(m->cycles * 1000000ULL / STUDIO2_CPU_CLOCK), &d))
        d = studio2_machine_console_read(m, a);
    if (m->watch)
    {
        const int bk = kind == CDP1802_READ_DMA ? STUDIO2_BUS_DMA : STUDIO2_BUS_MEMR;
        const uint8_t want = kind == CDP1802_READ_DMA ? STUDIO2_WATCH_DMA : STUDIO2_WATCH_MEMR;
        if ((m->watch & want) && m->bus_hook)
            m->bus_hook(m, m->bus_hook_user, bk, a, d);
    }
    return d;
}

static void bus_write(void *user, uint16_t a, uint8_t d)
{
    studio2_machine_t *m = user;

    /* map(0x0000, 0x01ff).mirror(0xfc00).ram(), minus machine_start's
     * unmap of $0000-$07FF */
    if (a >= 0x0800 && !(a & 0x0200))
        m->ram[a & (STUDIO2_RAM_SIZE - 1)] = d;
    if ((m->watch & STUDIO2_WATCH_MEMW) && m->bus_hook)
        m->bus_hook(m, m->bus_hook_user, STUDIO2_BUS_MEMW, a, d);
}

/* studio2_io_map: port 1 read pulses the 1861's display-on (dispon_r), port
 * 2 write is the key latch; the rest is unmapped (high). */
static uint8_t io_read(void *user, int port)
{
    studio2_machine_t *m = user;
    uint8_t d = 0xff;

    if (port == 1)
    {
        cdp1861_disp_on_w(&m->vdc, 1);
        cdp1861_disp_on_w(&m->vdc, 0);
        m->inp1_count++;
    }
    if ((m->watch & STUDIO2_WATCH_IOR) && m->bus_hook)
        m->bus_hook(m, m->bus_hook_user, STUDIO2_BUS_IOR, (uint16_t)port, d);
    return d;
}

static void io_write(void *user, int port, uint8_t data)
{
    studio2_machine_t *m = user;

    if (port == 2)
        m->keylatch = data & 0x0f;
    m->last_out[port & 7] = data;
    if ((m->watch & STUDIO2_WATCH_IOW) && m->bus_hook)
        m->bus_hook(m, m->bus_hook_user, STUDIO2_BUS_IOW, (uint16_t)port, data);
}

/* EF1 is the 1861's EFx; EF2 is not connected; EF3/EF4 are the latched key
 * of keypad A/B (BIT(port, keylatch): keys 10-15 do not exist). */
static int ef_read(void *user, int n)
{
    studio2_machine_t *m = user;

    switch (n)
    {
    case 0: return m->vdc.efx_line;
    case 2: return (m->keys[STUDIO2_KEYPAD_A] >> m->keylatch) & 1;
    case 3: return (m->keys[STUDIO2_KEYPAD_B] >> m->keylatch) & 1;
    default: return 0;
    }
}

/*-------------------------------------------------
    the beeper (beep.cpp)
-------------------------------------------------*/

/* The samples due up to the current clock, with the beeper as it stood
 * over them: called before every CPU pass, so a Q change lands on the
 * sample MAME's stream update would put it on. */
static void audio_advance(studio2_machine_t *m)
{
    const uint64_t due = m->cycles * STUDIO2_AUDIO_RATE / STUDIO2_CPU_CLOCK;

    while (m->audio_samples < due)
    {
        float v = 0.0f;
        if (m->beep_enable)
        {
            m->beep_incr -= STUDIO2_BEEP_HZ;
            while (m->beep_incr < 0)
            {
                m->beep_incr += STUDIO2_AUDIO_RATE / 2;
                m->beep_signal = -m->beep_signal;
            }
            v = m->beep_signal * m->beep_gain;
        }
        if (m->out_frames < STUDIO2_AUDIO_MAX)
        {
            m->out[2 * m->out_frames] = v;
            m->out[2 * m->out_frames + 1] = v;
            m->out_frames++;
        }
        m->audio_samples++;
    }
}

static void q_write(void *user, int q)
{
    studio2_machine_t *m = user;
    bool on = q != 0;

    if (m->beep_enable == on)
        return;
    m->beep_enable = on;
    /* restart wave from beginning */
    m->beep_incr = 0;
    m->beep_signal = 1.0f;
}

static void dma_write(void *user, uint16_t addr, uint8_t data)
{
    studio2_machine_t *m = user;
    cdp1861_dma_w(&m->vdc, m->cycles, addr, data);
}

/*-------------------------------------------------
    power-on
-------------------------------------------------*/

void studio2_machine_init(studio2_machine_t *m, const uint8_t *bios, uint32_t bios_size,
                          studio2_cart_t *cart, bool mame_power_on)
{
    const cdp1802_bus_t bus = {
        m, bus_read, bus_write, io_read, io_write, ef_read, q_write, dma_write,
    };

    memset(m, 0, sizeof *m);
    m->cart = cart;
    if (bios && bios_size >= STUDIO2_ROM_SERVED)
    {
        memcpy(m->rom, bios, bios_size < STUDIO2_BIOS_SIZE ? bios_size : STUDIO2_BIOS_SIZE);
        m->have_bios = true;
    }
    else
    {
        memcpy(m->rom, studio2_stub, STUDIO2_STUB_SIZE);
    }

    m->beep_signal = 1.0f;
    m->beep_gain = 0.25f;

    cdp1861_init(&m->vdc);
    cdp1802_init(&m->cpu, &bus);
    /* The console's reset circuit holds CLEAR low at power-on: Q off, IE on.
     * MAME's CPU never sees that (its first pass is INIT straight from the
     * constructor's mode), which the firmware's Tier B reference corrects by
     * pulsing CLEAR; mame_power_on keeps MAME's way for A/B runs. */
    if (!mame_power_on)
        cdp1802_reset_state(&m->cpu);
    cdp1861_reset(&m->vdc, 0);
    m->clear_live = m->clear_held = false;
    m->frame_end = STUDIO2_FRAME_CLOCKS;
}

/*-------------------------------------------------
    running
-------------------------------------------------*/

static int pass(studio2_machine_t *m, bool hooks)
{
    int clocks;

    /* every 1861 timer due at or before this state's start has fired */
    cdp1861_run_until(&m->vdc, m->cycles);
    m->cpu.irq = m->vdc.int_line;
    m->cpu.dmaout = m->vdc.dma_line;
    audio_advance(m);

    if (m->cpu.at_fetch)
    {
        if (!m->have_bios && m->cpu.pc == STUDIO2_STUB_PARK)
            m->stub_parked = true;
        if (hooks && m->instr_hook)
            m->instr_hook(m, m->instr_hook_user);
    }

    clocks = cdp1802_step(&m->cpu);
    m->cycles += (uint64_t)clocks;
    return clocks;
}

int studio2_machine_step(studio2_machine_t *m)
{
    return pass(m, true);
}

int studio2_machine_step_quiet(studio2_machine_t *m)
{
    return pass(m, false);
}

void studio2_machine_step_instruction(studio2_machine_t *m)
{
    /* through the instruction under way to the next boundary; under CLEAR
     * (no instructions) one clock */
    int guard = 0;
    do
    {
        pass(m, false);
    } while (!m->cpu.at_fetch && m->cpu.mode == CDP1802_MODE_RUN && ++guard < 64);
}

void studio2_machine_snapshot(studio2_machine_t *m, uint32_t *dst)
{
    if (m->stub_parked)
        memset(dst, 0, sizeof m->fb);
    else
        cdp1861_snapshot(&m->vdc, dst);
}

void studio2_machine_run_frame(studio2_machine_t *m)
{
    while (m->cycles < m->frame_end)
        pass(m, true);
    /* VBLANK: screen_update */
    studio2_machine_snapshot(m, m->fb);
    while (m->frame_end <= m->cycles)
        m->frame_end += STUDIO2_FRAME_CLOCKS;
    m->frame_count++;
}

void studio2_machine_latch_inputs(studio2_machine_t *m)
{
    m->keys[STUDIO2_KEYPAD_A] = m->keys_live[STUDIO2_KEYPAD_A] & STUDIO2_KEYS_MASK;
    m->keys[STUDIO2_KEYPAD_B] = m->keys_live[STUDIO2_KEYPAD_B] & STUDIO2_KEYS_MASK;

    if (m->clear_live && !m->clear_held)
    {
        /* reset_w on the press: machine_reset() resets the 1861; clear_r
         * holds the CPU in RESET from its next pass */
        cdp1861_reset(&m->vdc, m->cycles);
        m->stub_parked = false;
    }
    m->clear_held = m->clear_live;
    m->cpu.clear = !m->clear_held;
}

/*-------------------------------------------------
    debugger access
-------------------------------------------------*/

uint8_t studio2_machine_peek(studio2_machine_t *m, uint16_t a)
{
    uint8_t d;

    if (m->cart && studio2_cart_peek(m->cart, a, &d))
        return d;
    return studio2_machine_console_read(m, a);
}

void studio2_machine_poke(studio2_machine_t *m, uint16_t a, uint8_t d)
{
    if (m->cart && studio2_cart_poke(m->cart, a, d))
        return;
    if (a >= 0x0800 && !(a & 0x0200))
        m->ram[a & (STUDIO2_RAM_SIZE - 1)] = d;
}
