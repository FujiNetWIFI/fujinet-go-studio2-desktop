/*
 * The Win32 Controllers window (F9): the RCA Studio II's top as a player sees
 * it -- the two recessed ten-key keypads (A left, B right), the gold centre
 * panel with its RCA badge, "Studio II" script and HOME TV PROGRAMMER, the
 * ribbed panel with the red LED and the black CLEAR bar, and the CONFIG and
 * STOP buttons the real console does not have -- drawn from the session's
 * layout (studio2session_controls_layout), so every frontend shows the same
 * parts. Under the console: the gamepads with the keypad each drives, and
 * Map mode.
 *
 * MOUSE: a press on a key or CLEAR holds it for as long as the button is
 * down: the console reads a keypad one key at a time, so a value present only
 * for the instant of a click falls between scans. Presses are the raw
 * button-down and button-up, never a click. The mouse is captured on press;
 * letting go anywhere, losing the capture, hiding, Map being armed or
 * deactivating the window releases whatever the pointer holds (a CLEAR that
 * stays down holds the CPU in reset). CONFIG and STOP fire on release over the
 * button, like a real one.
 *
 * What the console sees held -- keyboard, gamepads and this window together
 * (studio2session_target_held) -- is polled every 40 ms and painted in the
 * accent with black ink, and the LED glows while CLEAR is down, so the window
 * doubles as a gamepad tester.
 *
 * FIXED SIZE: laid out once, in a caption-only tool window with no thick frame
 * and no maximize box, so neither Map mode nor a long gamepad name changes its
 * size.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control to pick the target,
 * then press a host key OR a gamepad button; Esc cancels. While armed the
 * keys show what drives them (key / pad button), the picked target is lit and
 * the rest are drawn at 40%, and typing does not reach the console. Clear
 * takes the picked target's key and button away. "Show bindings" shows the
 * same legends outside Map mode. F9, F11 and F12 are the app's and cannot be
 * bound.
 *
 * The art is drawn at twice the size into a memory bitmap and shrunk with
 * HALFTONE (GDI has no anti-aliasing of its own); the text goes on after at the
 * window's own size.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include "../help/help_compat.h"
#include "../help/help_window.h"
#include "../key_forward.h"

#define PANEL_CLASS   "FujiNetGoStudio2Controllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi. */
#define UNIT96     36       /* pixels per layout unit */
#define MARGIN     12
#define CASE_PAD   14
#define HEAD_H     22
#define ROW_H      26       /* a gamepad row: its name and its Automatic / Keypad A / Keypad B choice */
#define ROW_GAP    3
#define PAD_ROWS   4        /* gamepad rows the window has room for */
#define BTN_H      28
#define HINT_H     20
#define SEGS       3
#define MAXCTL     64

#define IDT_CAPTURE 1
#define IDT_HELD    2
#define IDT_PADS    3

enum { SB_MAP, SB_CLEAR, SB_DEFAULTS, SB_SHOW, SB_HELP, SB_COUNT };

typedef struct {
    unsigned generation;
    int count, assign[PAD_ROWS], effective[PAD_ROWS];
} pads_state;

static HWND g_panel, g_parent;
static studio2session *g_session;
static int g_dpi = 96;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static int g_show_bindings;
static WPARAM g_swallow_vkey;           /* the key Map took: its repeats and release too */
static char g_hint[300];
static int g_hover_part = -1;           /* the part under the idle pointer */
static int g_tracking;

/* The layout (units), and where it sits in the client. */
static const studio2_control *g_layout;
static int g_numlayout;
static float g_layw, g_layh;
static double g_unit, g_orgx, g_orgy;

/* What the pointer holds: a pressed key, CLEAR or console button (layout index). */
static int g_mouse_part = -1;

/* What was last painted. */
static unsigned char g_lit[MAXCTL];
static pads_state g_pads_shown;
static int g_total_w, g_total_h;

static RECT g_case_rc, g_gp_rc, g_gp_head_rc, g_gp_row_rc[PAD_ROWS];
static RECT g_btn_rc[SB_COUNT], g_hint_rc;

static HFONT g_font, g_bold, g_caption, g_digit, g_tiny, g_script, g_badge, g_tag, g_sys;

static void refresh_held(void);

static int SCL(int val) { return studio2_scale(val, g_dpi); }

/* ---- fonts ------------------------------------------------------------------ */

static void free_fonts(void)
{
    HFONT *const slots[] = { &g_font, &g_bold, &g_caption, &g_digit, &g_tiny, &g_script,
                             &g_badge, &g_tag, &g_sys };
    size_t idx;
    for (idx = 0; idx < sizeof slots / sizeof slots[0]; idx++)
        if (*slots[idx]) { DeleteObject(*slots[idx]); *slots[idx] = NULL; }
}

static HFONT face_font(int px, int weight, int italic, const wchar_t *face)
{
    return CreateFontW(-MulDiv(px, g_dpi, 96), 0, 0, 0, weight, italic, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       face);
}

static void make_fonts(void)
{
    free_fonts();
    g_font = studio2_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_bold = studio2_make_font(g_dpi, 0, FW_BOLD, NULL);
    g_caption = studio2_make_font(g_dpi, 14, FW_BOLD, NULL);    /* A and B on the case */
    g_digit = studio2_make_font(g_dpi, 14, FW_BOLD, NULL);      /* the key legends */
    g_tiny = studio2_make_font(g_dpi, 9, FW_BOLD, NULL);        /* binding legends */
    g_sys = studio2_make_font(g_dpi, 9, FW_BOLD, NULL);
    g_script = face_font(22, FW_NORMAL, TRUE, L"Georgia");      /* "Studio II" */
    g_badge = face_font(11, FW_BOLD, TRUE, L"Arial");           /* "RCA" */
    g_tag = studio2_make_font(g_dpi, 8, FW_BOLD, NULL);         /* HOME TV PROGRAMMER */
}

/* ---- text -------------------------------------------------------------------- */

/* UTF-8 (legends, key names, gamepad names) through DrawTextW. */
static int text_out(HDC hdc, const char *utf8, RECT *rect, UINT fmt)
{
    wchar_t wide[256];
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof wide / sizeof wide[0])) <= 0) return 0;
    return DrawTextW(hdc, wide, -1, rect, fmt | DT_NOPREFIX);
}

static void text_line(HDC hdc, const char *utf8, const RECT *rect, UINT fmt)
{
    RECT copy = *rect;
    text_out(hdc, utf8, &copy, fmt | DT_SINGLELINE | DT_VCENTER);
}

/* Colours live here and only here; the geometry is the core's. From the photos
 * of the real console (DESIGN-studio2.md). */
#define COL_ACCENT     RGB(0xC8, 0xB5, 0x8C)              /* STUDIO2SESSION_ACCENT_RGB */
#define COL_ON_ACCENT  RGB(0x00, 0x00, 0x00)
#define COL_ACCENT_DK  RGB(0x7A, 0x66, 0x52)              /* STUDIO2SESSION_ACCENT_DARK_RGB: outlines */
#define COL_CASE_TOP   RGB(0xCF, 0xBE, 0x96)
#define COL_CASE_BOT   RGB(0xC0, 0xAC, 0x80)
#define COL_CASE_EDGE  RGB(0xA8, 0x94, 0x6A)
#define COL_CASE_SIDE  RGB(0x6E, 0x66, 0x48)              /* the darker band under the edge */
#define COL_CASE_HI    RGB(0xDD, 0xCF, 0xAA)
#define COL_CASE_INK   RGB(0xE6, 0xD9, 0xB6)              /* the raised A and B */
#define COL_BEZEL      RGB(0x1C, 0x1A, 0x16)
#define COL_ALU        RGB(0xC0, 0xC1, 0xBB)
#define COL_ALU_LINE   RGB(0xB0, 0xB1, 0xAB)
#define COL_KEY        RGB(0x2A, 0x28, 0x22)
#define COL_KEY_HI     RGB(0x45, 0x43, 0x3C)
#define COL_KEY_SIDE   RGB(0x12, 0x11, 0x0E)
#define COL_KEY_INK    RGB(0xF2, 0xF2, 0xEC)
#define COL_MARK       RGB(0xF2, 0xF2, 0xEC)
#define COL_MARK_EDGE  RGB(0x88, 0x89, 0x84)
#define COL_GOLD       RGB(0xC4, 0xB5, 0x8C)
#define COL_GOLD_LINE  RGB(0xB9, 0xAA, 0x84)
#define COL_RIBBED     RGB(0xB3, 0xAC, 0x7C)
#define COL_RIB_HI     RGB(0xC0, 0xB9, 0x8C)
#define COL_CHROME     RGB(0xCD, 0xCE, 0xC1)
#define COL_CHROME_DK  RGB(0x8A, 0x8B, 0x84)
#define COL_SILVER     RGB(0xCD, 0xCE, 0xC1)
#define COL_LED_OFF    RGB(0x8E, 0x1E, 0x1A)
#define COL_LED_ON     RGB(0xE8, 0x37, 0x2E)
#define COL_INK        RGB(0x1A, 0x18, 0x14)
#define COL_BADGE_BG   RGB(0xEC, 0xE6, 0xD2)
#define COL_BADGE_INK  RGB(0x6A, 0x5A, 0x38)
#define COL_SYS        RGB(0x1C, 0x1A, 0x16)
#define COL_SYS_INK    RGB(0xE4, 0xDB, 0xBE)
#define COL_DIM_TO     RGB(0xC0, 0xBF, 0xB0)              /* unpicked parts fade toward the panel */
#define NOFILL         ((COLORREF)0xFFFFFFFFu)

/* ---- drawing primitives ---------------------------------------------------------
 * The art is drawn at g_art_k times the window's size into a bitmap that holds
 * the pixel rectangle starting at (g_art_x, g_art_y). */
static int g_art_k = 1, g_art_x, g_art_y, g_art_rows;

static int art_xpos(double val) { return (int)floor((val - g_art_x) * g_art_k + 0.5); }
static int art_ypos(double val) { return (int)floor((val - g_art_y) * g_art_k + 0.5); }
static int art_len(double val) { return (int)floor(val * g_art_k + 0.5); }

static COLORREF mix_color(COLORREF start, COLORREF finish, double frac)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return RGB((int)(GetRValue(start) + (GetRValue(finish) - GetRValue(start)) * frac + 0.5),
               (int)(GetGValue(start) + (GetGValue(finish) - GetGValue(start)) * frac + 0.5),
               (int)(GetBValue(start) + (GetBValue(finish) - GetBValue(start)) * frac + 0.5));
}

/* An unpicked part in Map mode: 40% of its own colours over the panel's. */
static COLORREF tone(COLORREF col, int dim)
{
    return dim ? mix_color(col, COL_DIM_TO, 0.6) : col;
}

static void poly(HDC hdc, const POINT *pnts, int num, COLORREF fill, COLORREF line, double width)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Polygon(hdc, pnts, num);
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void ellipse(HDC hdc, double ctrx, double ctry, double radx, double rady, COLORREF fill, COLORREF line,
                    double width)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Ellipse(hdc, art_xpos(ctrx - radx), art_ypos(ctry - rady), art_xpos(ctrx + radx), art_ypos(ctry + rady));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius, COLORREF fill,
                  COLORREF line, double linew)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, linew > 0 ? art_len(linew) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, linew > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    RoundRect(hdc, art_xpos(xpos), art_ypos(ypos), art_xpos(xpos + width), art_ypos(ypos + height),
              art_len(radius * 2), art_len(radius * 2));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void line_seg(HDC hdc, double x0, double y0, double x1, double y1, COLORREF col, double width)
{
    HPEN pen = CreatePen(PS_SOLID, art_len(width) > 0 ? art_len(width) : 1, col);
    HGDIOBJ oldp = SelectObject(hdc, pen);
    MoveToEx(hdc, art_xpos(x0), art_ypos(y0), NULL);
    LineTo(hdc, art_xpos(x1), art_ypos(y1));
    SelectObject(hdc, oldp);
    DeleteObject(pen);
}

/* A rounded rectangle filled with a vertical gradient, then outlined. */
static void grad_rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius,
                       COLORREF top, COLORREF bottom, COLORREF edge, double edgew)
{
    const int left = art_xpos(xpos), right = art_xpos(xpos + width);
    const int first = art_ypos(ypos), last = art_ypos(ypos + height);
    HRGN clip = CreateRoundRectRgn(left, first, right + 1, last + 1, art_len(radius * 2), art_len(radius * 2));
    int row;
    HGDIOBJ oldb;
    SelectClipRgn(hdc, clip);
    oldb = SelectObject(hdc, GetStockObject(DC_BRUSH));
    for (row = first < 0 ? 0 : first; row <= last && row < g_art_rows; row++) {
        SetDCBrushColor(hdc, mix_color(top, bottom, last > first ? (double)(row - first) / (last - first) : 0));
        PatBlt(hdc, left, row, right - left + 1, 1, PATCOPY);
    }
    SelectObject(hdc, oldb);
    SelectClipRgn(hdc, NULL);
    DeleteObject(clip);
    if (edgew > 0) rrect(hdc, xpos, ypos, width, height, radius, NOFILL, edge, edgew);
}

/* The pixel rectangle of a layout part. */
static void part_box(const studio2_control *ctl, double *xpos, double *ypos, double *width, double *height)
{
    *xpos = g_orgx + (double)ctl->xpos * g_unit;
    *ypos = g_orgy + (double)ctl->ypos * g_unit;
    *width = (double)ctl->width * g_unit;
    *height = (double)ctl->height * g_unit;
}

static void part_rect(const studio2_control *ctl, RECT *rect)
{
    double xpos, ypos, width, height;
    part_box(ctl, &xpos, &ypos, &width, &height);
    SetRect(rect, (int)floor(xpos), (int)floor(ypos), (int)ceil(xpos + width), (int)ceil(ypos + height));
}

/* ---- what a part shows ---------------------------------------------------------- */

static int binding_mode(void)
{
    return g_map_state != -2 || g_show_bindings;
}

static int pressable(const studio2_control *ctl)
{
    return ctl->kind == STUDIO2_CTL_KEY || ctl->kind == STUDIO2_CTL_CLEAR || ctl->kind == STUDIO2_CTL_SYSBUTTON;
}

/* Shown held: what the console sees, in the accent. In Map mode the picked
 * target is the one lit. */
static int part_lit(int idx)
{
    const studio2_control *ctl = &g_layout[idx];
    if (g_map_state != -2)
        return g_map_state >= 0 && pressable(ctl) && ctl->target == g_map_state;
    if (ctl->kind == STUDIO2_CTL_SYSBUTTON) return 0;
    return idx < MAXCTL && g_lit[idx];
}

/* Map mode with a target picked: every other control is drawn at 40%. */
static int part_dim(const studio2_control *ctl)
{
    if (g_map_state < 0) return 0;
    return pressable(ctl) && ctl->target != g_map_state;
}

/* What drives a target, as "Key" and "Pad" lines ("" when unbound). */
static void binding_lines(int target, char *key, int keysz, char *pad, int padsz)
{
    char both[160], *sep;
    key[0] = pad[0] = '\0';
    if (target < 0) return;
    studio2session_binding_text(g_session, target, both, sizeof both);
    sep = strstr(both, " / ");
    if (sep) {
        *sep = '\0';
        snprintf(pad, (size_t)padsz, "%.*s", padsz - 1, sep + 3);
    }
    snprintf(key, (size_t)keysz, "%.*s", keysz - 1, both);
    /* the keys are small: "Keypad 5" reads "KP 5" */
    if (strncmp(key, "Keypad ", 7) == 0) {
        char shortname[64];
        snprintf(shortname, sizeof shortname, "KP %.50s", key + 7);
        snprintf(key, (size_t)keysz, "%s", shortname);
    }
}

/* ---- the art -------------------------------------------------------------------- */

/* The small triangles printed on a keypad's plate: up above the 2, left beside
 * the 4, right beside the 6, down below the 8, diagonals at the corners. */
static void tri(HDC hdc, double ctrx, double ctry, double ang, double size, COLORREF col)
{
    POINT pnts[3];
    const double rad = ang * 3.14159265358979323846 / 180.0, dirx = cos(rad), diry = -sin(rad);
    pnts[0].x = art_xpos(ctrx + dirx * size);
    pnts[0].y = art_ypos(ctry + diry * size);
    pnts[1].x = art_xpos(ctrx - dirx * size * 0.6 - diry * size * 0.8);
    pnts[1].y = art_ypos(ctry - diry * size * 0.6 + dirx * size * 0.8);
    pnts[2].x = art_xpos(ctrx - dirx * size * 0.6 + diry * size * 0.8);
    pnts[2].y = art_ypos(ctry - diry * size * 0.6 - dirx * size * 0.8);
    poly(hdc, pnts, 3, col, COL_MARK_EDGE, 0.4);
}

static void art_well(HDC hdc, const studio2_control *ctl)
{
    double xpos, ypos, width, height, y;
    const double inset = g_unit * 0.14, size = g_unit * 0.11;
    const double kx = g_orgx + (ctl->xpos + 0.575) * g_unit;   /* the key grid, from the layout */
    const double ky = g_orgy + (ctl->ypos + 0.5) * g_unit;
    const double col0 = kx + g_unit * 1.25 / 2, pitch = g_unit * 1.6, rowp = g_unit * 1.5;
    part_box(ctl, &xpos, &ypos, &width, &height);
    rrect(hdc, xpos, ypos, width, height, g_unit * 0.18, COL_BEZEL, COL_BEZEL, 0);
    rrect(hdc, xpos + inset, ypos + inset, width - 2 * inset, height - 2 * inset, g_unit * 0.06, COL_ALU, COL_MARK_EDGE,
          0.6);
    /* the brushed aluminium: hairlines across the plate */
    for (y = ypos + inset + g_unit * 0.1; y < ypos + height - inset - g_unit * 0.05; y += g_unit * 0.12)
        line_seg(hdc, xpos + inset + g_unit * 0.04, y, xpos + width - inset - g_unit * 0.04, y, COL_ALU_LINE, 0.4);
    /* direction marks */
    tri(hdc, col0 + pitch, ky - g_unit * 0.3, 90, size, COL_MARK);                    /* above the 2 */
    tri(hdc, kx - g_unit * 0.3, ky + rowp + g_unit * 0.5, 180, size, COL_MARK);       /* beside the 4 */
    tri(hdc, col0 + 2 * pitch + g_unit * 0.95, ky + rowp + g_unit * 0.5, 0, size, COL_MARK);   /* beside the 6 */
    tri(hdc, col0 + pitch, ky + 2 * rowp + g_unit * 1.25, 270, size, COL_MARK);       /* below the 8 */
    tri(hdc, kx - g_unit * 0.3, ky - g_unit * 0.25, 135, size, COL_MARK);             /* corners: 1 3 7 9 */
    tri(hdc, col0 + 2 * pitch + g_unit * 0.95, ky - g_unit * 0.25, 45, size, COL_MARK);
    tri(hdc, kx - g_unit * 0.3, ky + 2 * rowp + g_unit * 1.2, 225, size, COL_MARK);
    tri(hdc, col0 + 2 * pitch + g_unit * 0.95, ky + 2 * rowp + g_unit * 1.2, 315, size, COL_MARK);
}

static void art_panel(HDC hdc, const studio2_control *ctl)
{
    double xpos, ypos, width, height, pos;
    const double unit = g_unit;
    part_box(ctl, &xpos, &ypos, &width, &height);
    if (strcmp(ctl->label, "gold") == 0) {
        /* chrome trim round the whole centre panel (gold + ribbed), drawn with the gold plate */
        double total = height;
        int other;
        for (other = 0; other < g_numlayout; other++)
            if (g_layout[other].kind == STUDIO2_CTL_PANEL && strcmp(g_layout[other].label, "ribbed") == 0)
                total += (double)g_layout[other].height * unit;
        rrect(hdc, xpos - unit * 0.1, ypos - unit * 0.1, width + unit * 0.2, total + unit * 0.2,
              unit * 0.14, COL_CHROME, COL_CHROME_DK, 0.8);
        rrect(hdc, xpos, ypos, width, height, unit * 0.06, COL_GOLD, COL_GOLD_LINE, 0.6);
        for (pos = ypos + unit * 0.07; pos < ypos + height - unit * 0.03; pos += unit * 0.09)
            line_seg(hdc, xpos + unit * 0.03, pos, xpos + width - unit * 0.03, pos, COL_GOLD_LINE, 0.35);
    } else if (strcmp(ctl->label, "ribbed") == 0) {
        rrect(hdc, xpos, ypos, width, height, unit * 0.06, COL_RIBBED, COL_GOLD_LINE, 0.6);
        for (pos = xpos + unit * 0.12; pos < xpos + width - unit * 0.06; pos += unit * 0.2) {
            line_seg(hdc, pos, ypos + unit * 0.1, pos, ypos + height - unit * 0.1, COL_GOLD_LINE, 0.7);
            line_seg(hdc, pos + unit * 0.07, ypos + unit * 0.1, pos + unit * 0.07, ypos + height - unit * 0.1,
                     COL_RIB_HI, 0.5);
        }
    } else {                                       /* silver */
        rrect(hdc, xpos - unit * 0.05, ypos - unit * 0.05, width + unit * 0.1, height + unit * 0.1, unit * 0.04,
              COL_CHROME_DK, COL_CHROME_DK, 0);
        rrect(hdc, xpos, ypos, width, height, unit * 0.03, COL_SILVER, COL_MARK_EDGE, 0.5);
    }
}

static void art_key(HDC hdc, int idx)
{
    const studio2_control *ctl = &g_layout[idx];
    double xpos, ypos, width, height;
    const int lit = part_lit(idx), dim = part_dim(ctl);
    const int pressed = idx == g_mouse_part && g_map_state == -2;
    const double side = (double)SCL(3);
    part_box(ctl, &xpos, &ypos, &width, &height);
    if (ctl->kind == STUDIO2_CTL_SYSBUTTON) {
        grad_rrect(hdc, xpos, ypos + (pressed ? 1 : 0), width, height - (pressed ? 1 : 0), g_unit * 0.14,
                   lit ? COL_ACCENT : tone(mix_color(COL_SYS, COL_KEY_HI, 0.5), dim),
                   lit ? COL_ACCENT : tone(COL_SYS, dim), lit ? COL_ACCENT_DK : tone(COL_KEY_SIDE, dim), 1.2);
        return;
    }
    if (ctl->kind == STUDIO2_CTL_CLEAR) {
        /* the wide flat black bar */
        rrect(hdc, xpos, ypos + side * 0.6, width, height - side * 0.6, g_unit * 0.1, tone(COL_KEY_SIDE, dim),
              tone(COL_KEY_SIDE, dim), 0);
        grad_rrect(hdc, xpos, ypos + (pressed ? side * 0.4 : 0), width, height - side * 0.6, g_unit * 0.1,
                   lit ? COL_ACCENT : tone(COL_KEY_HI, dim), lit ? COL_ACCENT : tone(COL_KEY, dim),
                   lit ? COL_ACCENT_DK : tone(COL_KEY_SIDE, dim), lit ? 2.0 : 1.0);
        return;
    }
    rrect(hdc, xpos, ypos + side, width, height - side, g_unit * 0.1, tone(COL_KEY_SIDE, dim), tone(COL_KEY_SIDE, dim),
          0);
    grad_rrect(hdc, xpos, ypos + (pressed ? side * 0.5 : 0), width, height - side, g_unit * 0.1,
               lit ? COL_ACCENT : tone(COL_KEY_HI, dim), lit ? COL_ACCENT : tone(COL_KEY, dim),
               lit ? COL_ACCENT_DK : tone(COL_KEY_SIDE, dim), lit ? 2.0 : 1.0);
}

static void art_led(HDC hdc, const studio2_control *ctl, int lit)
{
    double xpos, ypos, width, height;
    part_box(ctl, &xpos, &ypos, &width, &height);
    {
        const double ctrx = xpos + width / 2, ctry = ypos + height / 2, rad = width / 2;
        ellipse(hdc, ctrx, ctry, rad * 1.05, rad * 1.05, COL_CHROME, COL_CHROME_DK, 0.8);   /* the chrome bezel */
        if (lit) ellipse(hdc, ctrx, ctry, rad * 0.9, rad * 0.9, mix_color(COL_LED_ON, COL_CHROME, 0.4), COL_LED_ON, 0);
        ellipse(hdc, ctrx, ctry, rad * 0.62, rad * 0.62, lit ? COL_LED_ON : COL_LED_OFF, COL_BEZEL, 0.5);
        ellipse(hdc, ctrx - rad * 0.2, ctry - rad * 0.2, rad * 0.16, rad * 0.16,
                lit ? RGB(0xFF, 0xC0, 0xB8) : RGB(0xC0, 0x5A, 0x52), RGB(0xC0, 0x5A, 0x52), 0);
    }
}

static void art_case(HDC hdc)
{
    const double ww = g_case_rc.right - g_case_rc.left, hh = g_case_rc.bottom - g_case_rc.top;
    const double band = (double)SCL(7);
    int idx;
    /* the olive-brown band under the edge, then the khaki top bevelled over it */
    rrect(hdc, g_case_rc.left, g_case_rc.top + band, ww, hh - band, SCL(22), COL_CASE_SIDE, COL_CASE_SIDE, 0);
    grad_rrect(hdc, g_case_rc.left, g_case_rc.top, ww, hh - band, SCL(22), COL_CASE_TOP, COL_CASE_BOT, COL_CASE_EDGE,
               1.5);
    rrect(hdc, g_case_rc.left + SCL(7), g_case_rc.top + SCL(7), ww - SCL(14), hh - band - SCL(14), SCL(16), NOFILL,
          COL_CASE_HI, 1.0);

    for (idx = 0; idx < g_numlayout; idx++) {
        const studio2_control *ctl = &g_layout[idx];
        if (ctl->kind == STUDIO2_CTL_WELL) art_well(hdc, ctl);
        else if (ctl->kind == STUDIO2_CTL_PANEL) art_panel(hdc, ctl);
        else if (ctl->kind == STUDIO2_CTL_CAPTION && strcmp(ctl->label, "RCA") == 0) {
            double xpos, ypos, width, height;
            part_box(ctl, &xpos, &ypos, &width, &height);
            rrect(hdc, xpos, ypos, width, height, g_unit * 0.04, COL_BADGE_BG, COL_GOLD_LINE, 1.0);
        } else if (ctl->kind == STUDIO2_CTL_LED) {
            art_led(hdc, ctl, g_map_state == -2 && idx < MAXCTL && g_lit[idx]);
        } else if (pressable(ctl)) art_key(hdc, idx);
    }
}

/* ---- the text ------------------------------------------------------------------- */

static void text_parts(HDC hdc)
{
    int idx;
    SetBkMode(hdc, TRANSPARENT);
    for (idx = 0; idx < g_numlayout; idx++) {
        const studio2_control *ctl = &g_layout[idx];
        RECT rect;
        const int lit = part_lit(idx), dim = part_dim(ctl);
        const int pressed = idx == g_mouse_part && g_map_state == -2;
        part_rect(ctl, &rect);
        switch (ctl->kind) {
        case STUDIO2_CTL_CAPTION:
            if (strcmp(ctl->label, "A") == 0 || strcmp(ctl->label, "B") == 0) {
                RECT shadow = rect;
                OffsetRect(&shadow, 1, 1);
                SelectObject(hdc, g_caption);
                SetTextColor(hdc, COL_CASE_EDGE);
                text_line(hdc, ctl->label, &shadow, DT_CENTER);
                SetTextColor(hdc, COL_CASE_INK);
                text_line(hdc, ctl->label, &rect, DT_CENTER);
            } else if (strcmp(ctl->label, "RCA") == 0) {
                SelectObject(hdc, g_badge);
                SetTextColor(hdc, COL_BADGE_INK);
                text_line(hdc, ctl->label, &rect, DT_CENTER);
            } else if (strcmp(ctl->label, "Studio II") == 0) {
                SelectObject(hdc, g_script);
                SetTextColor(hdc, COL_INK);
                text_line(hdc, ctl->label, &rect, DT_CENTER | DT_NOCLIP);
            } else if (strcmp(ctl->label, "HOME TV PROGRAMMER") == 0) {
                SelectObject(hdc, g_tag);
                SetTextColor(hdc, COL_INK);
                SetTextCharacterExtra(hdc, 1);
                text_line(hdc, ctl->label, &rect, DT_CENTER | DT_NOCLIP);
                SetTextCharacterExtra(hdc, 0);
            } else {                                   /* CLEAR, printed on the plate under the bar */
                SelectObject(hdc, g_sys);
                SetTextColor(hdc, tone(COL_INK, g_map_state >= 0));
                text_line(hdc, ctl->label, &rect, DT_CENTER);
            }
            break;
        case STUDIO2_CTL_KEY:
        case STUDIO2_CTL_CLEAR:
        case STUDIO2_CTL_SYSBUTTON: {
            RECT face = rect;
            COLORREF ink;
            if (ctl->kind == STUDIO2_CTL_SYSBUTTON) ink = lit ? COL_ON_ACCENT : tone(COL_SYS_INK, dim);
            else ink = lit ? COL_ON_ACCENT : tone(COL_KEY_INK, dim);
            if (ctl->kind == STUDIO2_CTL_KEY) face.bottom -= SCL(3);
            else if (ctl->kind == STUDIO2_CTL_CLEAR) face.bottom -= SCL(2);
            if (pressed) OffsetRect(&face, 0, 1);
            SetTextColor(hdc, ink);
            if (binding_mode()) {
                char key[64], pad[64];
                RECT upper = face, lower = face;
                binding_lines(ctl->target, key, sizeof key, pad, sizeof pad);
                if (!key[0] && !pad[0]) snprintf(key, sizeof key, "%s", "\xe2\x80\x94");
                SelectObject(hdc, g_tiny);
                if (ctl->kind == STUDIO2_CTL_SYSBUTTON) {
                    /* the label over what drives it, both tiny, on the small black button */
                    const int mid = (face.top + face.bottom) / 2;
                    upper.bottom = mid + SCL(1);
                    lower.top = mid - SCL(1);
                    text_line(hdc, ctl->label, &upper, DT_CENTER);
                    text_line(hdc, key, &lower, DT_CENTER | DT_END_ELLIPSIS);
                } else if (pad[0]) {
                    const int mid = (face.top + face.bottom) / 2;
                    upper.bottom = mid + SCL(1);
                    lower.top = mid - SCL(1);
                    text_line(hdc, key, &upper, DT_CENTER | DT_END_ELLIPSIS);
                    text_line(hdc, pad, &lower, DT_CENTER | DT_END_ELLIPSIS);
                } else {
                    text_line(hdc, key, &face, DT_CENTER | DT_END_ELLIPSIS);
                }
            } else if (ctl->kind == STUDIO2_CTL_KEY) {
                SelectObject(hdc, g_digit);
                text_line(hdc, ctl->label, &face, DT_CENTER);
            } else if (ctl->kind == STUDIO2_CTL_SYSBUTTON) {
                SelectObject(hdc, g_sys);
                text_line(hdc, ctl->label, &face, DT_CENTER);
            }
            break;
        }
        default:
            break;
        }
    }
}

/* ---- the strip: gamepads, Map, hint --------------------------------------------- */

static void draw_face(HDC hdc, const RECT *bounds, int pushed, int accent, int inactive)
{
    RECT rect = *bounds;
    if (accent) {
        HBRUSH back = CreateSolidBrush(COL_ACCENT);
        HBRUSH edge = CreateSolidBrush(COL_ACCENT_DK);
        FillRect(hdc, &rect, back);
        FrameRect(hdc, &rect, edge);
        DeleteObject(back);
        DeleteObject(edge);
        SetTextColor(hdc, COL_ON_ACCENT);
    } else {
        DrawFrameControl(hdc, &rect, DFC_BUTTON,
                         DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0) | (inactive ? DFCS_INACTIVE : 0));
        SetTextColor(hdc, GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }
}

static const char *strip_label(int btn)
{
    switch (btn) {
    case SB_MAP: return g_map_state == -2 ? "Map" : "Done";
    case SB_CLEAR: return "Clear";
    case SB_DEFAULTS: return "Defaults";
    case SB_SHOW: return "Show bindings";
    default: return "Help";
    }
}

/* The segment of a pad row: 0 is Automatic, 1 Keypad A, 2 Keypad B. */
static void segment_rect(int row, int seg, RECT *rect)
{
    const RECT *rowrc = &g_gp_row_rc[row];
    const int gap = SCL(3), widths[SEGS] = { SCL(76), SCL(70), SCL(70) };
    const int total = widths[0] + widths[1] + widths[2] + 2 * gap;
    int left = rowrc->right - total, i;
    for (i = 0; i < seg; i++) left += widths[i] + gap;
    SetRect(rect, left, rowrc->top, left + widths[seg], rowrc->bottom - 1);
}

/* One line per gamepad: its name and the keypad it drives now, then the
 * Automatic / Keypad A / Keypad B choice, the current one in the accent. */
static void paint_gamepads(HDC hdc)
{
    static const char *const segs[SEGS] = { "Automatic", "Keypad A", "Keypad B" };
    const int num = studio2session_gamepad_count(g_session);
    int idx, seg;

    SelectObject(hdc, g_bold);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    text_line(hdc, "Gamepads", &g_gp_head_rc, DT_LEFT);
    SelectObject(hdc, g_font);
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    if (num == 0) {
        text_line(hdc, "No gamepads connected - plug one in", &g_gp_row_rc[0], DT_LEFT);
        return;
    }
    if (num > PAD_ROWS) {
        char more[48];
        snprintf(more, sizeof more, "%d more not shown", num - PAD_ROWS);
        text_line(hdc, more, &g_gp_head_rc, DT_RIGHT);
    }
    for (idx = 0; idx < num && idx < PAD_ROWS; idx++) {
        const int assign = studio2session_gamepad_assignment(g_session, idx);
        const int effective = studio2session_gamepad_effective_port(g_session, idx);
        const int chosen = assign < 0 || assign > 1 ? 0 : assign + 1;
        char name[128], line[200];
        RECT name_rc, first;

        segment_rect(idx, 0, &first);
        name_rc = g_gp_row_rc[idx];
        name_rc.right = first.left - SCL(6);
        studio2session_gamepad_name(g_session, idx, name, sizeof name);
        if (effective >= 0) snprintf(line, sizeof line, "%s  (keypad %c)", name, 'A' + effective);
        else snprintf(line, sizeof line, "%s  (no keypad)", name);
        SelectObject(hdc, g_font);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        text_line(hdc, line, &name_rc, DT_LEFT | DT_END_ELLIPSIS);
        for (seg = 0; seg < SEGS; seg++) {
            RECT rect;
            segment_rect(idx, seg, &rect);
            draw_face(hdc, &rect, 0, seg == chosen, 0);
            text_line(hdc, segs[seg], &rect, DT_CENTER);
        }
    }
}

/* What the hint line says about the part under an idle pointer. */
static void hover_text(int part, char *out, int outsz)
{
    const studio2_control *ctl;
    char key[64], pad[64];
    out[0] = '\0';
    if (part < 0 || part >= g_numlayout) return;
    ctl = &g_layout[part];
    if (ctl->target < 0) return;
    binding_lines(ctl->target, key, sizeof key, pad, sizeof pad);
    snprintf(out, (size_t)outsz, "%s   Key: %s   Gamepad: %s", studio2_target_name(ctl->target),
             key[0] ? key : "none", pad[0] ? pad : "none");
}

static void paint_strip(HDC hdc)
{
    int btn;
    char hover[200];
    SelectObject(hdc, g_font);
    for (btn = 0; btn < SB_COUNT; btn++) {
        const int armed = (btn == SB_MAP && g_map_state != -2) || (btn == SB_SHOW && g_show_bindings);
        const int inactive = btn == SB_CLEAR && g_map_state < 0;
        draw_face(hdc, &g_btn_rc[btn], 0, armed, inactive);
        text_line(hdc, strip_label(btn), &g_btn_rc[btn], DT_CENTER);
    }
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    hover_text(g_map_state >= 0 ? -1 : g_hover_part, hover, sizeof hover);
    text_line(hdc, hover[0] ? hover : g_hint, &g_hint_rc, DT_LEFT | DT_END_ELLIPSIS);
}

/* The whole window: the console's art supersampled, its text, the strips.
 * Only what the update region needs is drawn. */
static void paint_rect(HDC hdc, const RECT *inval)
{
    RECT art;
    const int scale = 2;

    FillRect(hdc, inval, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(hdc, TRANSPARENT);

    IntersectRect(&art, inval, &g_case_rc);
    if (!IsRectEmpty(&art)) {
        const int width = art.right - art.left, height = art.bottom - art.top;
        BITMAPINFO bmi;
        void *bits = NULL;
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = width * scale;
        bmi.bmiHeader.biHeight = -height * scale;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp) {
            HBRUSH face = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            RECT all = { 0, 0, width * scale, height * scale };
            HGDIOBJ old = SelectObject(mem, bmp);
            FillRect(mem, &all, face);
            DeleteObject(face);
            SetBkMode(mem, TRANSPARENT);
            g_art_k = scale;
            g_art_x = art.left;
            g_art_y = art.top;
            g_art_rows = height * scale;
            art_case(mem);
            g_art_k = 1;
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, NULL);
            StretchBlt(hdc, art.left, art.top, width, height, mem, 0, 0, width * scale, height * scale, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
        }
        DeleteDC(mem);
        SelectObject(hdc, g_font);
        text_parts(hdc);
    }
    paint_gamepads(hdc);
    paint_strip(hdc);
}

/* ---- pressing ------------------------------------------------------------------- */

static void invalidate_part(int part)
{
    RECT rect;
    if (!g_panel || part < 0 || part >= g_numlayout) return;
    part_rect(&g_layout[part], &rect);
    InflateRect(&rect, SCL(14), SCL(14));
    InvalidateRect(g_panel, &rect, FALSE);
}

/* Lets go of what the pointer holds in this window (only that: the main
 * window releases its own keys). CONFIG and STOP fire on release inside the
 * button, like any button. */
static void mouse_release(int fire_sys)
{
    const int part = g_mouse_part;
    g_mouse_part = -1;
    if (g_session) studio2session_release_pointer(g_session);
    if (part >= 0 && part < g_numlayout) {
        const studio2_control *ctl = &g_layout[part];
        if (fire_sys && ctl->kind == STUDIO2_CTL_SYSBUTTON)
            studio2session_sysaction_post(g_session, ctl->target - STUDIO2_TARGET_SYSACT(0));
        invalidate_part(part);
    }
}

static void set_hint(const char *text)
{
    snprintf(g_hint, sizeof g_hint, "%s", text);
    g_hover_part = -1;
    if (g_panel) InvalidateRect(g_panel, &g_hint_rc, FALSE);
}

#define IDLE_HINT "Press a key or CLEAR to hold it; hover a key to see what drives it."

static void set_map_state(int state)
{
    g_map_state = state;
    if (state != -2) {
        mouse_release(0);                       /* a held CLEAR must not outlive Map being armed */
        studio2session_release_all(g_session);  /* Map swallows key-ups, so drop keyboard holds too */
    }
    if (state < 0) {
        if (g_panel) KillTimer(g_panel, IDT_CAPTURE);
        studio2session_gamepad_capture_cancel(g_session);
    }
    if (state == -2) {
        set_hint(IDLE_HINT);
    } else if (state == -1) {
        set_hint("Click a control to remap it. Esc finishes.");
    } else {
        char msg[200];
        snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s. Esc cancels.", studio2_target_name(state));
        set_hint(msg);
        studio2session_gamepad_capture_begin(g_session);
        if (g_panel) SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    memset(g_lit, 0, sizeof g_lit);
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* After a rebind: what it is now, and whom it was taken from. */
static void bound_hint(int target, const char *what, const char *was)
{
    char msg[300], name[96];
    snprintf(name, sizeof name, "%s", studio2_target_name(target));   /* a static buffer */
    if (was && was[0]) snprintf(msg, sizeof msg, "Bound %s to %s (was %s)", name, what, was);
    else snprintf(msg, sizeof msg, "Bound %s to %s", name, what);
    set_hint(msg);
}

/* ---- layout --------------------------------------------------------------------- */

/* The case with the keypads and the centre panel in it, then the gamepad rows
 * and the strip: the window is this size always. */
static void layout_static(void)
{
    const int left = SCL(MARGIN);
    int ypos = SCL(MARGIN), idx, xpos, content_w;
    static const int widths[SB_COUNT] = { 52, 54, 72, 108, 54 };

    g_layout = studio2session_controls_layout(&g_numlayout, &g_layw, &g_layh);
    g_unit = (double)UNIT96 * g_dpi / 96.0;
    content_w = (int)ceil((double)g_layw * g_unit) + 2 * SCL(CASE_PAD);
    g_total_w = 2 * left + content_w;

    SetRect(&g_case_rc, left, ypos, left + content_w, 0);
    g_orgx = g_case_rc.left + SCL(CASE_PAD);
    g_orgy = g_case_rc.top + SCL(CASE_PAD);
    g_case_rc.bottom = (int)ceil(g_orgy + (double)g_layh * g_unit) + SCL(CASE_PAD) + SCL(7);
    ypos = g_case_rc.bottom + SCL(10);

    SetRect(&g_gp_head_rc, left, ypos, left + content_w, ypos + SCL(HEAD_H));
    ypos += SCL(HEAD_H);
    for (idx = 0; idx < PAD_ROWS; idx++) {
        SetRect(&g_gp_row_rc[idx], left, ypos, left + content_w, ypos + SCL(ROW_H));
        ypos += SCL(ROW_H + ROW_GAP);
    }
    SetRect(&g_gp_rc, left, g_gp_head_rc.top, left + content_w, ypos);
    ypos += SCL(6);

    xpos = left;
    for (idx = 0; idx < SB_COUNT; idx++) {
        if (idx == SB_HELP) xpos = left + content_w - SCL(widths[idx]);
        SetRect(&g_btn_rc[idx], xpos, ypos, xpos + SCL(widths[idx]), ypos + SCL(BTN_H));
        xpos += SCL(widths[idx]) + SCL(6);
    }
    ypos += SCL(BTN_H) + SCL(6);
    SetRect(&g_hint_rc, left, ypos, left + content_w, ypos + SCL(HINT_H));
    g_total_h = ypos + SCL(HINT_H) + SCL(MARGIN);
}

static void rebuild(void)
{
    mouse_release(0);
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    make_fonts();
    layout_static();
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* ---- polling -------------------------------------------------------------------- */

/* What the console sees held, lit; the LED. */
static void refresh_held(void)
{
    int idx;
    if (!g_panel || g_map_state != -2) return;
    for (idx = 0; idx < g_numlayout && idx < MAXCTL; idx++) {
        const studio2_control *ctl = &g_layout[idx];
        unsigned char now;
        if (ctl->kind != STUDIO2_CTL_KEY && ctl->kind != STUDIO2_CTL_CLEAR && ctl->kind != STUDIO2_CTL_LED) continue;
        now = (unsigned char)(studio2session_target_held(g_session, ctl->target) != 0);
        if (now != g_lit[idx]) {
            g_lit[idx] = now;
            invalidate_part(idx);
        }
    }
}

/* Hot-plugged pads and auto-assignment changes: the rows are rebuilt. */
static void refresh_pads(void)
{
    pads_state now;
    int idx;
    memset(&now, 0, sizeof now);
    now.generation = studio2session_gamepad_generation(g_session);
    now.count = studio2session_gamepad_count(g_session);
    for (idx = 0; idx < PAD_ROWS && idx < now.count; idx++) {
        now.assign[idx] = studio2session_gamepad_assignment(g_session, idx);
        now.effective[idx] = studio2session_gamepad_effective_port(g_session, idx);
    }
    if (memcmp(&now, &g_pads_shown, sizeof now) != 0) {
        g_pads_shown = now;
        InvalidateRect(g_panel, &g_gp_rc, FALSE);
    }
}

/* ---- keys ----------------------------------------------------------------------- */

static void hide_panel(void)
{
    set_map_state(-2);
    mouse_release(0);
    if (GetCapture() == g_panel) ReleaseCapture();
    g_swallow_vkey = 0;
    ShowWindow(g_panel, SW_HIDE);
}

/* The app's own keys, never the console's or Map's. */
static int app_key(WPARAM vkey)
{
    return vkey == VK_F9 || vkey == VK_F11 || vkey == VK_F12;
}

/* A key for the panel (its own window proc is the only caller): Map first,
 * then the bindings. Returns 1 if it was consumed. A WM_SYSKEY* is the
 * console's only when a binding takes it (Left Alt is keypad A's 0 by
 * default); Alt+F4, the menu mnemonics and Alt+Esc stay Windows', and a
 * session action is never fired from a system key. */
static int panel_key(UINT msg, WPARAM wparam, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const int sys = msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
    const int repeat = (lparam & (1 << 30)) != 0;
    const uint32_t ks = studio2_keysym_from_msg(wparam, lparam);

    /* the key Map took, its repeats and its release */
    if (g_swallow_vkey && wparam == g_swallow_vkey) {
        if (!down) g_swallow_vkey = 0;
        return 1;
    }
    if (app_key(wparam)) {
        if (g_map_state >= 0) {
            if (down && !repeat) set_hint("F9, F11 and F12 belong to the app and cannot be bound.");
            g_swallow_vkey = wparam;
            return 1;
        }
        if (down && !repeat) {
            if (wparam == VK_F9) hide_panel();
            else if (g_parent) SendMessageA(g_parent, WM_KEYDOWN, wparam, lparam);   /* the main window's own */
        }
        return 1;
    }
    if (wparam == VK_F10) return 0;
    if (msg == WM_SYSKEYDOWN && wparam == VK_F4) return 0;

    if (g_map_state != -2) {
        if (!down || repeat) return 1;
        if (wparam == VK_ESCAPE) {
            set_map_state(g_map_state >= 0 ? -1 : -2);
            g_swallow_vkey = wparam;
            return 1;
        }
        if (g_map_state >= 0 && ks) {
            char was[128], name[48];
            const int target = g_map_state;
            was[0] = '\0';
            g_swallow_vkey = wparam;
            studio2session_binding_set_key(g_session, target, ks, was, sizeof was);
            studio2session_keysym_name(ks, name, sizeof name);
            set_map_state(-1);          /* stay armed: remapping several in a row is normal */
            bound_hint(target, name, was);
            InvalidateRect(g_panel, NULL, FALSE);
        }
        return 1;
    }

    if (!ks) return 0;
    if (down) {
        if (repeat) return 1;
        if (!sys) {
            const int sa = studio2session_key_sysaction(g_session, ks);
            if (sa >= 0) { studio2session_sysaction_post(g_session, sa); return 1; }
        }
        return studio2session_key(g_session, ks, 1) != 0;
    }
    /* A bound Alt's release stops here, so it does not open the system menu. */
    return studio2session_key(g_session, ks, 0) != 0;
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wnd, client;
    GetWindowRect(g_panel, &wnd);
    GetClientRect(g_panel, &client);
    if (client.right == g_total_w && client.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wnd.right - wnd.left) - client.right + g_total_w,
                 (wnd.bottom - wnd.top) - client.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The window's outer size for the laid-out client. */
static void window_size(HWND hwnd, SIZE *dims)
{
    RECT wnd, client;
    if (hwnd && GetWindowRect(hwnd, &wnd) && GetClientRect(hwnd, &client) && client.right > 0) {
        dims->cx = (wnd.right - wnd.left) - client.right + g_total_w;
        dims->cy = (wnd.bottom - wnd.top) - client.bottom + g_total_h;
    } else {
        SetRect(&wnd, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wnd, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        dims->cx = wnd.right - wnd.left;
        dims->cy = wnd.bottom - wnd.top;
    }
}

static void rescale(int dpi)
{
    g_dpi = dpi;
    rebuild();
}

/* ---- the mouse ------------------------------------------------------------------ */

/* The layout part under a client point, or -1. */
static int part_at(POINT pnt)
{
    if (!g_layout) return -1;
    return studio2session_controls_hit((float)((pnt.x - g_orgx) / g_unit), (float)((pnt.y - g_orgy) / g_unit));
}

static void mouse_down(HWND hwnd, POINT pnt)
{
    int part, row, seg, btn;
    SetFocus(hwnd);
    if (g_hover_part >= 0) {
        g_hover_part = -1;
        InvalidateRect(hwnd, &g_hint_rc, FALSE);
    }

    for (btn = 0; btn < SB_COUNT; btn++) {
        if (!PtInRect(&g_btn_rc[btn], pnt)) continue;
        switch (btn) {
        case SB_MAP: set_map_state(g_map_state == -2 ? -1 : -2); return;
        case SB_CLEAR:
            if (g_map_state >= 0) {
                studio2session_binding_clear(g_session, g_map_state);
                set_hint("Cleared. Press a key or a gamepad button to bind it again");
                studio2session_gamepad_capture_begin(g_session);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return;
        case SB_DEFAULTS:
            studio2session_bindings_reset(g_session);
            set_hint("Every key and button is back to its default");
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_SHOW:
            g_show_bindings = !g_show_bindings;
            studio2session_set_int(g_session, "controllers_show_bindings", g_show_bindings);
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_HELP: studio2_help_show(hwnd, "controllers"); return;
        default: return;
        }
    }
    for (row = 0; row < PAD_ROWS && row < studio2session_gamepad_count(g_session); row++)
        for (seg = 0; seg < SEGS; seg++) {
            RECT rect;
            segment_rect(row, seg, &rect);
            if (PtInRect(&rect, pnt)) {
                /* 0 = automatic (connection order), 1 = keypad A, 2 = keypad B */
                studio2session_gamepad_assign(g_session, row, seg - 1);
                refresh_pads();
                InvalidateRect(hwnd, &g_gp_rc, FALSE);
                return;
            }
        }

    part = part_at(pnt);
    if (part < 0) return;
    {
        const studio2_control *ctl = &g_layout[part];
        if (g_map_state != -2) {
            /* armed: picks (or changes) the target */
            if (ctl->target >= 0) set_map_state(ctl->target);
            return;
        }
        mouse_release(0);
        g_mouse_part = part;
        if (ctl->kind != STUDIO2_CTL_SYSBUTTON) studio2session_pointer_press(g_session, ctl->target, 1);
        invalidate_part(part);
        SetCapture(hwnd);
        refresh_held();
    }
}

static void mouse_move(HWND hwnd, POINT pnt)
{
    if (g_mouse_part >= 0) {
        /* dragged off what it holds: let go, as a button does */
        if (part_at(pnt) != g_mouse_part) {
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            refresh_held();
        }
        return;
    }
    {   /* idle: the hint line says what drives the part under the pointer */
        const int part = part_at(pnt);
        if (!g_tracking) {
            TRACKMOUSEEVENT track;
            memset(&track, 0, sizeof track);
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            g_tracking = TrackMouseEvent(&track) != 0;
        }
        if (part != g_hover_part) {
            g_hover_part = part;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
    }
}

static void mouse_up(HWND hwnd)
{
    if (g_mouse_part >= 0) {
        /* CONFIG and STOP fire only if the release is over them */
        POINT pnt;
        int fire = 0;
        if (GetCursorPos(&pnt) && ScreenToClient(hwnd, &pnt)) fire = part_at(pnt) == g_mouse_part;
        mouse_release(fire);
        refresh_held();
    }
    if (GetCapture() == hwnd) ReleaseCapture();
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        /* Off-screen first: the held highlights repaint at 25 Hz. */
        RECT inval = paint.rcPaint;
        const int width = inval.right - inval.left, height = inval.bottom - inval.top;
        if (width > 0 && height > 0) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, width, height);
            HGDIOBJ oldbmp = SelectObject(mem, bmp);
            HGDIOBJ oldfont = SelectObject(mem, g_font);
            SetViewportOrgEx(mem, -inval.left, -inval.top, NULL);
            paint_rect(mem, &inval);
            SetViewportOrgEx(mem, 0, 0, NULL);
            BitBlt(hdc, inval.left, inval.top, width, height, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldfont);
            SelectObject(mem, oldbmp);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_down(hwnd, pnt);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_move(hwnd, pnt);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = 0;
        if (g_hover_part >= 0) {
            g_hover_part = -1;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        /* released wherever the mouse ended up: capture means this arrives
         * even if the pointer left the part */
        mouse_up(hwnd);
        return 0;
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go, without
         * firing a button nobody finished clicking */
        if ((HWND)lparam != hwnd) {
            mouse_release(0);
            refresh_held();
        }
        return 0;

    case WM_TIMER:
        if (wparam == IDT_CAPTURE) {
            int button;
            if (g_map_state < 0) {
                KillTimer(hwnd, IDT_CAPTURE);
            } else if (studio2session_gamepad_capture_poll(g_session, &button)) {
                char was[128];
                const int target = g_map_state;
                was[0] = '\0';
                studio2session_binding_set_button(g_session, target, button, was, sizeof was);
                set_map_state(-1);          /* stay armed: remapping several in a row is normal */
                bound_hint(target, studio2_pad_button_name(button), was);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        } else if (wparam == IDT_HELD) {
            refresh_held();
        } else if (wparam == IDT_PADS) {
            refresh_pads();
        }
        return 0;

    /* WM_SYSKEY* too: a bound Alt is the console's */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wparam, lparam)) return 0;
        break;
    case WM_SYSCHAR:
        /* Alt+letter: the main window's menu mnemonics, as when it has the focus */
        if (wparam != ' ' && g_parent) { SendMessageA(g_parent, msg, wparam, lparam); return 0; }
        break;
    case WM_ENTERMENULOOP:
        studio2session_release_all(g_session);
        break;

    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            const HWND other = (HWND)lparam;
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            g_swallow_vkey = 0;     /* its release goes elsewhere */
            /* Leaving the app: nothing typed here stays held (within it, the
             * window that gets the key-up lets go). */
            if (!other || GetWindowThreadProcessId(other, NULL) != GetCurrentThreadId())
                studio2session_release_all(g_session);
            refresh_held();
        }
        break;                      /* DefWindowProc gives the window the focus */
    case WM_SHOWWINDOW:
        if (wparam) {
            refresh_held();
            SetTimer(hwnd, IDT_HELD, 40, NULL);
            SetTimer(hwnd, IDT_PADS, 250, NULL);
        } else {
            KillTimer(hwnd, IDT_HELD);
            KillTimer(hwnd, IDT_PADS);
            mouse_release(0);
        }
        break;
    case WM_GETMINMAXINFO:
        /* Fixed: the size limits say so too, so neither maximizing nor a
         * compositor (Wine passes them on) makes it any other size. */
        if (g_total_w) {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            SIZE dims;
            window_size(hwnd, &dims);
            limits->ptMinTrackSize.x = limits->ptMaxTrackSize.x = limits->ptMaxSize.x = dims.cx;
            limits->ptMinTrackSize.y = limits->ptMaxTrackSize.y = limits->ptMaxSize.y = dims.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *suggested = (const RECT *)lparam;
        rescale(HIWORD(wparam));
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: a remap and the position survive closing it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        /* with the main window: the next toggle starts afresh */
        mouse_release(0);
        set_map_state(-2);
        g_panel = NULL;
        free_fonts();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static int create_panel(void)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want;

    g_show_bindings = studio2session_get_int(g_session, "controllers_show_bindings", 0) > 0;

    if (!registered) {
        WNDCLASSEXA wcls;
        memset(&wcls, 0, sizeof wcls);
        wcls.cbSize = sizeof wcls;
        wcls.lpfnWndProc = panel_proc;      /* no CS_DBLCLKS: fast clicks stay press/release pairs */
        wcls.hInstance = inst;
        wcls.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wcls.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wcls) != 0;
    }
    g_dpi = studio2_window_dpi(g_parent);
    make_fonts();
    layout_static();

    SetRect(&want, 0, 0, g_total_w, g_total_h);
    /* A tool window with a caption and no thick frame or maximize box: it is
     * exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers", PANEL_STYLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top,
                              g_parent, NULL, inst, NULL);
    if (!g_panel) return 0;
    studio2_help_set_context(g_panel, "controllers", 0);    /* F1 is bindable here */
    rescale(studio2_window_dpi(g_panel));    /* placed on another monitor than the main window */
    fit_client();
    memset(&g_pads_shown, 0, sizeof g_pads_shown);
    memset(g_lit, 0, sizeof g_lit);
    g_hover_part = -1;
    set_map_state(-2);
    return 1;
}

static void show_panel(void)
{
    refresh_pads();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

void studio2_controllers_show(HWND owner, studio2session *session)
{
    g_session = session;
    if (owner) g_parent = owner;
    if (!g_panel && !create_panel()) return;
    if (!IsWindowVisible(g_panel)) show_panel();
    else SetForegroundWindow(g_panel);
}

void studio2_controllers_toggle(HWND owner, studio2session *session)
{
    g_session = session;
    if (owner) g_parent = owner;
    if (!g_panel && !create_panel()) return;
    if (IsWindowVisible(g_panel)) hide_panel();
    else show_panel();
}

BOOL studio2_controllers_visible(void)
{
    return g_panel && IsWindowVisible(g_panel);
}

void studio2_controllers_refresh(void)
{
    if (!g_panel) return;
    if (IsWindowVisible(g_panel)) {
        refresh_pads();
        refresh_held();
        InvalidateRect(g_panel, NULL, FALSE);
    }
}
