/* cdp1861.c -- the CDP1861 (see cdp1861.h).
 *
 * Transposed from MAME's src/devices/video/cdp1861.cpp (BSD-3-Clause,
 * copyright-holders Curt Coder): device_reset, the int/efx/dma timer
 * callbacks, dma_w, disp_on_w and screen_update.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME; copyright-holders Curt Coder -- see COMPLIANCE.md)
 */

#include <string.h>

#include "cdp1861.h"

#define WHITE 0x00ffffffu
#define BLACK 0x00000000u

void cdp1861_init(cdp1861_t *v)
{
    memset(v, 0, sizeof *v);
}

/* screen_device::time_until_pos for this screen: the vblank origin is line
 * 0 (visarea bottom + 1 == 262), and a position the beam is already at or
 * past goes to the next frame. */
static uint64_t time_until_pos(uint64_t now, int vpos, int hpos)
{
    const int64_t target = (int64_t)vpos * CDP1861_SCREEN_WIDTH + hpos;
    int64_t cur = (int64_t)(now % CDP1861_FRAME_CLOCKS);
    int64_t delta = target - cur;

    if (delta <= 0)
        delta += CDP1861_FRAME_CLOCKS;
    return (uint64_t)delta;
}

void cdp1861_reset(cdp1861_t *v, uint64_t now)
{
    v->int_next = now + time_until_pos(now, CDP1861_SCANLINE_INT_START, 0);
    v->efx_next = now + time_until_pos(now, CDP1861_SCANLINE_EFX_TOP_START, 0);
    v->dma_next = now + CDP1861_CYCLES_DMA_START;

    v->disp = 0;
    v->dmaout = 0;
    v->dispon = 0;

    v->int_line = 0;
    v->dma_line = 0;
    v->efx_line = 0;
}

static void int_tick(cdp1861_t *v)
{
    const uint64_t now = v->int_next;

    if (cdp1861_vpos(now) == CDP1861_SCANLINE_INT_START)
    {
        if (v->disp)
            v->int_line = 1;
        v->int_next = now + time_until_pos(now, CDP1861_SCANLINE_INT_END, 0);
    }
    else
    {
        if (v->disp)
            v->int_line = 0;
        v->int_next = now + time_until_pos(now, CDP1861_SCANLINE_INT_START, 0);
    }
}

static void efx_tick(cdp1861_t *v)
{
    const uint64_t now = v->efx_next;

    switch (cdp1861_vpos(now))
    {
    case CDP1861_SCANLINE_EFX_TOP_START:
        v->efx_line = 1;
        v->efx_next = now + time_until_pos(now, CDP1861_SCANLINE_EFX_TOP_END, 0);
        break;
    case CDP1861_SCANLINE_EFX_TOP_END:
        v->efx_line = 0;
        v->efx_next = now + time_until_pos(now, CDP1861_SCANLINE_EFX_BOTTOM_START, 0);
        break;
    case CDP1861_SCANLINE_EFX_BOTTOM_START:
        v->efx_line = 1;
        v->efx_next = now + time_until_pos(now, CDP1861_SCANLINE_EFX_BOTTOM_END, 0);
        break;
    case CDP1861_SCANLINE_EFX_BOTTOM_END:
        v->efx_line = 0;
        v->efx_next = now + time_until_pos(now, CDP1861_SCANLINE_EFX_TOP_START, 0);
        break;
    default:
        /* MAME's switch has no default: the timer would never be re-armed.
         * It is only ever placed on those four lines. */
        v->efx_next = UINT64_MAX;
        break;
    }
}

static void dma_tick(cdp1861_t *v)
{
    const uint64_t now = v->dma_next;
    const int scanline = cdp1861_vpos(now);
    const int in_display = scanline >= CDP1861_SCANLINE_DISPLAY_START
                        && scanline < CDP1861_SCANLINE_DISPLAY_END;

    if (v->dmaout)
    {
        if (v->disp && in_display)
            v->dma_line = 0;
        v->dma_next = now + CDP1861_CYCLES_DMA_WAIT;
        v->dmaout = 0;
    }
    else
    {
        if (v->disp && in_display)
            v->dma_line = 1;
        v->dma_next = now + CDP1861_CYCLES_DMA_ACTIVE;
        v->dmaout = 1;
    }
}

uint64_t cdp1861_next_event(const cdp1861_t *v)
{
    uint64_t t = v->int_next;
    if (v->efx_next < t)
        t = v->efx_next;
    if (v->dma_next < t)
        t = v->dma_next;
    return t;
}

void cdp1861_run_until(cdp1861_t *v, uint64_t t)
{
    for (;;)
    {
        /* in MAME's list order for equal times: int, efx, dma (none of
         * them reads another's state, so the order is not observable) */
        uint64_t next = cdp1861_next_event(v);
        if (next > t)
            return;
        if (v->int_next == next)
            int_tick(v);
        else if (v->efx_next == next)
            efx_tick(v);
        else
            dma_tick(v);
    }
}

void cdp1861_dma_w(cdp1861_t *v, uint64_t now, uint16_t addr, uint8_t data)
{
    const int sx = cdp1861_hpos(now) + 4;
    const int y = cdp1861_vpos(now);
    const size_t base = (size_t)y * CDP1861_SCREEN_WIDTH + (size_t)sx;

    for (int x = 0; x < 8; x++)
    {
        /* MAME's pix(y, sx + x) on a 112-pixel row: past the row's end it
         * is the next row's start; past the bitmap's end nothing */
        if (base + (size_t)x < sizeof v->bitmap / sizeof v->bitmap[0])
            v->bitmap[base + (size_t)x] = (data & 0x80) ? WHITE : BLACK;
        data = (uint8_t)(data << 1);
    }
    v->dma_bytes++;
    v->last_dma_addr = addr;
}

void cdp1861_disp_on_w(cdp1861_t *v, int state)
{
    if (!v->dispon && state)
        v->disp = 1;
    v->dispon = state;
}

void cdp1861_snapshot(const cdp1861_t *v, uint32_t *dst)
{
    for (int y = 0; y < CDP1861_VIS_H; y++)
    {
        uint32_t *row = dst + (size_t)y * CDP1861_VIS_W;
        if (!v->disp)
        {
            for (int x = 0; x < CDP1861_VIS_W; x++)
                row[x] = BLACK;
            continue;
        }
        memcpy(row, v->bitmap + (size_t)(y + CDP1861_VIS_Y0) * CDP1861_SCREEN_WIDTH + CDP1861_VIS_X0,
               CDP1861_VIS_W * sizeof(uint32_t));
    }
}
