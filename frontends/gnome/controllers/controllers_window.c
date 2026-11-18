/*
 * The Controllers window: the console's top drawn with cairo from the core's
 * layout (studio2session_controls_layout), hit-tested with
 * studio2session_controls_hit.
 *
 * Every press is the raw press and release of a GtkGestureClick, not a
 * button's "clicked": the console reads a keypad one key at a time, polling,
 * so a value present only for the instant of a click falls between two reads.
 * A key on screen is HELD for as long as the mouse button is down -- and so is
 * CLEAR, which holds the CPU in reset for as long as it is down. The pointer's
 * holds are therefore let go (studio2session_release_pointer) on mouse-up, on
 * the gesture's cancel, on focus loss, when the window hides, and when Map is
 * armed or the window maps: a stuck CLEAR freezes the machine.
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it. Not
 * resizable: the canvas has a fixed size and the window floats at the size it
 * makes. Wrapped or long labels carry width floors so the toplevel never grows.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control to pick the target,
 * then press a keyboard key OR a gamepad button. Clear unbinds the picked
 * target; Defaults restores the table; Show bindings paints each key's binding
 * on it. While a target is picked, the others dim to 40%.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../window.h"
#include "../help/help_window.h"

#include <math.h>
#include <string.h>

#define SCALE   40.0        /* pixels per layout unit */
#define MARGIN  16.0

#define ACCENT_R  (((STUDIO2SESSION_ACCENT_RGB >> 16) & 255) / 255.0)
#define ACCENT_G  (((STUDIO2SESSION_ACCENT_RGB >> 8) & 255) / 255.0)
#define ACCENT_B  ((STUDIO2SESSION_ACCENT_RGB & 255) / 255.0)

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;
static studio2session *g_session;
static GtkWidget *g_area;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 the picked
 * target waiting for a key or pad button. */
static int g_map_state = -2;
static GtkWidget *g_map_button, *g_clear_button, *g_defaults_button, *g_show_check;
static GtkWidget *g_map_hint;
static guint g_capture_timer, g_held_timer, g_pad_timer;
static GtkWidget *g_pad_box;
static unsigned g_pad_generation = (unsigned)-1;
static gboolean g_sysact_down[STUDIO2_SYSACT_COUNT];
static int g_pointer_target = -1;     /* the target the mouse holds, or -1 */
static guint32 g_last_sig;
static gboolean g_show_bindings;

static void refresh_clear_button(void);

/* ---- helpers --------------------------------------------------------------- */

static void run_sysaction(int sa)
{
    if (g_parent)
        studio2_window_run_sysaction(STUDIO2_WINDOW(g_parent), sa);
    else
        studio2session_sysaction(g_session, sa);
}

static void release_pointer(void)
{
    if (g_pointer_target >= 0 || g_session) {
        if (g_session)
            studio2session_release_pointer(g_session);
    }
    g_pointer_target = -1;
    if (g_area)
        gtk_widget_queue_draw(g_area);
}

static void set_hint(const char *text)
{
    gtk_label_set_text(GTK_LABEL(g_map_hint), text);
}

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    studio2session_gamepad_capture_cancel(g_session);
}

static gboolean capture_tick(gpointer d)
{
    int button;
    (void)d;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (studio2session_gamepad_capture_poll(g_session, &button)) {
        char stolen[128], msg[200];
        studio2session_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        if (stolen[0])
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", studio2_pad_button_name(button), stolen);
        else
            g_snprintf(msg, sizeof msg, "Bound %s", studio2_pad_button_name(button));
        g_capture_timer = 0;
        /* stay armed on the same target: Clear and a second binding are next */
        set_hint(msg);
        studio2session_gamepad_capture_begin(g_session);
        g_capture_timer = g_timeout_add(50, capture_tick, NULL);
        gtk_widget_queue_draw(g_area);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    if (state != -2) {
        release_pointer();        /* a key held into Map would never be released */
        studio2session_release_all(g_session);   /* Map swallows key-ups, so drop keyboard holds too */
    }
    g_map_state = state;
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "studio2-accent");
        set_hint("");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "studio2-accent");
        set_hint("Click a control to remap");
    } else {
        char msg[128];
        g_snprintf(msg, sizeof msg, "Press a key or gamepad button for %s",
                   studio2_target_name(state));
        set_hint(msg);
        stop_capture();
        studio2session_gamepad_capture_begin(g_session);
        g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_clear_button();
    gtk_widget_queue_draw(g_area);
}

static void refresh_clear_button(void)
{
    gtk_widget_set_sensitive(g_clear_button, g_map_state >= 0);
}

/* ---- drawing ---------------------------------------------------------------- */

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static void rgb(cairo_t *cr, unsigned hex)
{
    cairo_set_source_rgb(cr, ((hex >> 16) & 255) / 255.0, ((hex >> 8) & 255) / 255.0,
                         (hex & 255) / 255.0);
}

/* Text centred in a box, shrunk to fit its width. */
static void text_fit(cairo_t *cr, const char *text, double cx, double cy, double maxw,
                     double size, cairo_font_slant_t slant, cairo_font_weight_t weight,
                     const char *face)
{
    cairo_text_extents_t ext;
    cairo_select_font_face(cr, face, slant, weight);
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, text, &ext);
    if (ext.width > maxw && ext.width > 0) {
        size *= maxw / ext.width;
        cairo_set_font_size(cr, size);
        cairo_text_extents(cr, text, &ext);
    }
    cairo_move_to(cr, cx - ext.width / 2 - ext.x_bearing, cy - ext.height / 2 - ext.y_bearing);
    cairo_show_text(cr, text);
}

static void tri(cairo_t *cr, double x, double y, double dx, double dy)
{
    /* a small triangle pointing along (dx, dy) */
    const double s = 4.0;
    cairo_move_to(cr, x + dx * s, y + dy * s);
    cairo_line_to(cr, x - dy * s - dx * s * 0.6, y + dx * s - dy * s * 0.6);
    cairo_line_to(cr, x + dy * s - dx * s * 0.6, y - dx * s - dy * s * 0.6);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void draw_well(cairo_t *cr, const studio2_control *c, double x, double y,
                      double w, double h)
{
    const double pad = 0.18 * SCALE;
    double px = x + pad, py = y + pad, pw = w - 2 * pad, ph = h - 2 * pad, k;
    (void)c;
    rgb(cr, 0x1C1A16);
    rrect(cr, x, y, w, h, 7);
    cairo_fill(cr);
    rgb(cr, 0xC0C1BB);
    rrect(cr, px, py, pw, ph, 3);
    cairo_fill(cr);
    /* brushed hairlines */
    rgb(cr, 0xB0B1AB);
    cairo_set_line_width(cr, 0.6);
    for (k = py + 3; k < py + ph; k += 3) {
        cairo_move_to(cr, px + 1, k + 0.5);
        cairo_line_to(cr, px + pw - 1, k + 0.5);
    }
    cairo_stroke(cr);
    /* the direction marks around the grid (games steer with 2 4 6 8, 1 3 7 9) */
    rgb(cr, 0x4A4A44);
    {
        const double u = SCALE, cx = x + w / 2;
        const double top = y + 0.33 * u, bot = y + 4.75 * u, mid = y + 2.5 * u;
        const double side = 2.55 * u;
        tri(cr, cx, top, 0, -1);
        tri(cr, cx, y + 4.72 * u, 0, 1);
        tri(cr, cx - side, mid, -1, 0);
        tri(cr, cx + side, mid, 1, 0);
        tri(cr, cx - side, top, -0.7, -0.7);
        tri(cr, cx + side, top, 0.7, -0.7);
        tri(cr, cx - side, bot, -0.7, 0.7);
        tri(cr, cx + side, bot, 0.7, 0.7);
    }
}

static void draw_panel(cairo_t *cr, const studio2_control *c, double x, double y,
                       double w, double h)
{
    if (!strcmp(c->label, "gold")) {
        /* the chrome trim around the whole centre panel is drawn with it */
        rgb(cr, 0xCDCEC1);
        rrect(cr, x - 4, y - 4, w + 8, 6.5 * SCALE + 8, 5);
        cairo_fill(cr);
        rgb(cr, 0xC4B58C);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
        /* a brushed sheen */
        rgb(cr, 0xB9AC89);
        cairo_set_line_width(cr, 0.6);
        for (double k = y + 2; k < y + h; k += 3) {
            cairo_move_to(cr, x, k + 0.5);
            cairo_line_to(cr, x + w, k + 0.5);
        }
        cairo_stroke(cr);
    } else if (!strcmp(c->label, "ribbed")) {
        rgb(cr, 0xB3AC7C);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
        rgb(cr, 0x978F62);
        cairo_set_line_width(cr, 1.0);
        for (double k = x + 3; k < x + w; k += 5) {
            cairo_move_to(cr, k + 0.5, y);
            cairo_line_to(cr, k + 0.5, y + h);
        }
        cairo_stroke(cr);
        rgb(cr, 0xCDCEC1);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, x, y + 0.75);
        cairo_line_to(cr, x + w, y + 0.75);
        cairo_stroke(cr);
    } else {            /* silver */
        rgb(cr, 0xCDCEC1);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
        rgb(cr, 0xBFC0B3);
        cairo_set_line_width(cr, 0.6);
        for (double k = y + 2; k < y + h; k += 3) {
            cairo_move_to(cr, x, k + 0.5);
            cairo_line_to(cr, x + w, k + 0.5);
        }
        cairo_stroke(cr);
    }
}

static void draw_caption(cairo_t *cr, const studio2_control *c, double x, double y,
                         double w, double h)
{
    const double cx = x + w / 2, cy = y + h / 2;
    if (!strcmp(c->label, "RCA")) {
        rgb(cr, 0xDDD2AE);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
        rgb(cr, 0xA89B73);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1, h - 1);
        cairo_stroke(cr);
        cairo_set_source_rgb(cr, 1, 1, 0.97);
        text_fit(cr, "RCA", cx, cy, w - 6, 14, CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_BOLD,
                 "Sans");
    } else if (!strcmp(c->label, "Studio II")) {
        rgb(cr, 0x1A1814);
        text_fit(cr, "Studio II", cx, cy, w, 30, CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_NORMAL,
                 "Serif");
    } else if (!strcmp(c->label, "HOME TV PROGRAMMER")) {
        rgb(cr, 0x1A1814);
        text_fit(cr, c->label, cx, cy, w, 9.5, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL,
                 "Sans");
    } else if (!strcmp(c->label, "CLEAR")) {
        rgb(cr, 0x1A1814);
        text_fit(cr, "CLEAR", cx, cy, w, 9, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL,
                 "Sans");
    } else {            /* the raised A and B in the case plastic */
        cairo_set_source_rgba(cr, 1, 1, 1, 0.45);
        text_fit(cr, c->label, cx + 0.7, cy + 0.7, w, 20, CAIRO_FONT_SLANT_NORMAL,
                 CAIRO_FONT_WEIGHT_NORMAL, "Sans");
        rgb(cr, 0x8E7C56);
        text_fit(cr, c->label, cx, cy, w, 20, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL,
                 "Sans");
    }
}

static void draw_led(cairo_t *cr, double x, double y, double w, double h, int lit)
{
    const double cx = x + w / 2, cy = y + h / 2, r = w / 2;
    cairo_pattern_t *p;
    if (lit) {
        p = cairo_pattern_create_radial(cx, cy, r * 0.5, cx, cy, r * 2.2);
        cairo_pattern_add_color_stop_rgba(p, 0, 0.91, 0.22, 0.18, 0.55);
        cairo_pattern_add_color_stop_rgba(p, 1, 0.91, 0.22, 0.18, 0);
        cairo_set_source(cr, p);
        cairo_arc(cr, cx, cy, r * 2.2, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_pattern_destroy(p);
    }
    rgb(cr, 0x9A9A95);                  /* the chrome bezel */
    cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
    cairo_fill(cr);
    rgb(cr, 0x6C6C68);
    cairo_set_line_width(cr, 1);
    cairo_arc(cr, cx, cy, r - 0.5, 0, 2 * G_PI);
    cairo_stroke(cr);
    rgb(cr, lit ? 0xE8372E : 0x8E1E1A);
    cairo_arc(cr, cx, cy, r * 0.62, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, lit ? 0.5 : 0.25);
    cairo_arc(cr, cx - r * 0.2, cy - r * 0.2, r * 0.18, 0, 2 * G_PI);
    cairo_fill(cr);
}

static void draw_key(cairo_t *cr, const studio2_control *c, double x, double y, double w,
                     double h, int held, int picked, double alpha)
{
    char text[96] = "";
    const gboolean mapping = g_map_state != -2;
    const gboolean bindings = mapping || g_show_bindings;

    if (bindings)
        studio2session_binding_text(g_session, c->target, text, sizeof text);

    cairo_push_group(cr);
    /* the key's shadow, then its face */
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    rrect(cr, x, y + 2, w, h, 5);
    cairo_fill(cr);
    if (held || picked) {
        cairo_set_source_rgb(cr, ACCENT_R, ACCENT_G, ACCENT_B);
    } else {
        cairo_pattern_t *p = cairo_pattern_create_linear(x, y, x, y + h);
        cairo_pattern_add_color_stop_rgb(p, 0, 0.22, 0.21, 0.18);
        cairo_pattern_add_color_stop_rgb(p, 1, 0.14, 0.13, 0.11);   /* #2A2822 avg */
        cairo_set_source(cr, p);
        rrect(cr, x, y, w, h, 5);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(p);
        goto outline;
    }
    rrect(cr, x, y, w, h, 5);
    cairo_fill_preserve(cr);
outline:
    if (held || picked) {
        rgb(cr, 0x7A6652);
        cairo_set_line_width(cr, 1.5);
    } else {
        rgb(cr, 0x0E0D0A);
        cairo_set_line_width(cr, 1);
    }
    cairo_stroke(cr);

    /* ink: black on the accent, off-white on the black key */
    if (held || picked) rgb(cr, 0x000000); else rgb(cr, 0xEDEDE4);
    if (bindings && g_map_state != -2 && text[0] == 0 && picked == 0) {
        /* Map shows an unbound control as a dash */
        strcpy(text, "\xe2\x80\x94");
    }
    if (bindings) {
        /* the legend small in the corner, the binding centred */
        const double ls = c->kind == STUDIO2_CTL_KEY ? 9 : 8;
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, ls);
        cairo_move_to(cr, x + 4, y + ls + 2);
        cairo_show_text(cr, c->label);
        text_fit(cr, text, x + w / 2, y + h * 0.62, w - 6, 12, CAIRO_FONT_SLANT_NORMAL,
                 CAIRO_FONT_WEIGHT_BOLD, "Sans");
    } else if (c->kind != STUDIO2_CTL_CLEAR) {   /* the word is printed under the bar */
        const double size = c->kind == STUDIO2_CTL_KEY ? 16 : 10;
        text_fit(cr, c->label, x + w / 2, y + h / 2, w - 6, size, CAIRO_FONT_SLANT_NORMAL,
                 CAIRO_FONT_WEIGHT_BOLD, "Sans");
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, alpha);
}

static void draw_console(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer d)
{
    int count, i;
    float lw, lh;
    const studio2_control *lay = studio2session_controls_layout(&count, &lw, &lh);
    (void)area; (void)d;

    /* the case: the dark olive underside band, then the tan top */
    rgb(cr, 0x6E6648);
    rrect(cr, 2, 4, width - 4, height - 6, 18);
    cairo_fill(cr);
    rgb(cr, 0xC8B58C);
    rrect(cr, 2, 2, width - 4, height - 12, 18);
    cairo_fill_preserve(cr);
    rgb(cr, 0xA8946A);
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);

    for (i = 0; i < count; i++) {
        const studio2_control *c = &lay[i];
        const double x = MARGIN + c->xpos * SCALE, y = MARGIN + c->ypos * SCALE;
        const double w = c->width * SCALE, h = c->height * SCALE;
        const int held = c->target >= 0 && studio2session_target_held(g_session, c->target);
        const int picked = g_map_state >= 0 && c->target == g_map_state;

        switch (c->kind) {
        case STUDIO2_CTL_WELL:    draw_well(cr, c, x, y, w, h); break;
        case STUDIO2_CTL_PANEL:   draw_panel(cr, c, x, y, w, h); break;
        case STUDIO2_CTL_CAPTION: draw_caption(cr, c, x, y, w, h); break;
        case STUDIO2_CTL_LED:
            draw_led(cr, x, y, w, h, studio2session_target_held(g_session, c->target));
            break;
        case STUDIO2_CTL_KEY:
        case STUDIO2_CTL_CLEAR:
        case STUDIO2_CTL_SYSBUTTON: {
            const int lit = held || (c->target == g_pointer_target && c->kind == STUDIO2_CTL_SYSBUTTON);
            draw_key(cr, c, x, y, w, h, lit, picked,
                     (g_map_state >= 0 && !picked) ? 0.4 : 1.0);
            break;
        }
        }
    }
}

/* A signature of everything that decides how the canvas looks. */
static guint32 paint_signature(void)
{
    guint32 sig = 0;
    int t;
    for (t = 0; t < STUDIO2_TARGET_SYSACT(0); t++)
        if (studio2session_target_held(g_session, t))
            sig |= 1u << t;
    return sig;
}

static gboolean held_tick(gpointer d)
{
    guint32 sig;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window))
        return G_SOURCE_CONTINUE;
    sig = paint_signature();
    if (sig != g_last_sig) {
        g_last_sig = sig;
        gtk_widget_queue_draw(g_area);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- the pointer -------------------------------------------------------------- */

static void on_pressed(GtkGestureClick *g, int n, double x, double y, gpointer d)
{
    const int idx = studio2session_controls_hit((float)((x - MARGIN) / SCALE),
                                                (float)((y - MARGIN) / SCALE));
    const studio2_control *c;
    (void)g; (void)n; (void)d;
    if (idx < 0)
        return;
    c = &studio2session_controls_layout(NULL, NULL, NULL)[idx];

    if (g_map_state != -2) {
        set_map_state(c->target);
        return;
    }
    if (c->target >= STUDIO2_TARGET_SYSACT(0)) {
        /* a system action fires on the press and holds nothing; the main
         * window gives the feedback (toast, debugger) */
        g_pointer_target = c->target;
        run_sysaction(c->target - STUDIO2_TARGET_SYSACT(0));
    } else {
        g_pointer_target = c->target;
        studio2session_pointer_press(g_session, c->target, 1);
    }
    gtk_widget_queue_draw(g_area);
}

static void on_released(GtkGestureClick *g, int n, double x, double y, gpointer d)
{
    (void)g; (void)n; (void)x; (void)y; (void)d;
    release_pointer();
}

static void on_cancelled(GtkGesture *g, GdkEventSequence *seq, gpointer d)
{
    (void)g; (void)seq; (void)d;
    release_pointer();
}

/* ---- gamepads ------------------------------------------------------------- */

static void on_assign_changed(GObject *o, GParamSpec *ps, gpointer user_data)
{
    int idx = GPOINTER_TO_INT(user_data);
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    (void)ps;
    /* 0 = automatic (connection order), 1 = keypad A, 2 = keypad B */
    studio2session_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
}

static void refresh_pads(void)
{
    static const char *const choices[] = { "Automatic", "Keypad A", "Keypad B", NULL };
    GtkWidget *child;
    int n, i;

    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);

    n = studio2session_gamepad_count(g_session);
    if (n == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(none), 0);
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *dd, *eff;
        char text[128], port[32];
        int assign = studio2session_gamepad_assignment(g_session, i);
        int effective = studio2session_gamepad_effective_port(g_session, i);

        studio2session_gamepad_name(g_session, i, text, sizeof text);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(name), 24);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 40);
        dd = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), assign < 0 ? 0 : (guint)assign + 1);
        g_signal_connect(dd, "notify::selected", G_CALLBACK(on_assign_changed), GINT_TO_POINTER(i));
        if (effective >= 0) g_snprintf(port, sizeof port, "drives keypad %c", 'A' + effective);
        else g_snprintf(port, sizeof port, "drives nothing");
        eff = gtk_label_new(port);
        gtk_widget_add_css_class(eff, "dim-label");
        gtk_label_set_width_chars(GTK_LABEL(eff), 16);
        gtk_label_set_xalign(GTK_LABEL(eff), 0);
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), dd);
        gtk_box_append(GTK_BOX(g_pad_box), row);
    }
}

static gboolean pad_tick(gpointer d)
{
    unsigned gen;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;
    gen = studio2session_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    }
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval,
                               guint code, GdkModifierType st, gpointer d)
{
    guint32 keysym = code >= 8 ? studio2session_keysym_from_evdev(code - 8) : 0;
    int sa;
    (void)c; (void)st; (void)d;
    if (!keysym) keysym = keyval;

    /* an open drop-down (a gamepad's keypad) has the arrows and Return */
    {
        GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
        if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
            return FALSE;
    }
    if (keyval == GDK_KEY_F9) {
        studio2_controllers_toggle(NULL, g_session);
        return TRUE;
    }
    if (g_map_state >= 0) {
        char stolen[128], msg[200], name[32];
        if (keyval == GDK_KEY_F11 || keyval == GDK_KEY_F12) {
            set_hint("F9, F11 and F12 can't be bound");
            return TRUE;
        }
        studio2session_binding_set_key(g_session, g_map_state, keysym, stolen, sizeof stolen);
        studio2session_keysym_name(keysym, name, sizeof name);
        if (stolen[0])
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", name, stolen);
        else
            g_snprintf(msg, sizeof msg, "Bound %s", name);
        set_hint(msg);
        gtk_widget_queue_draw(g_area);
        return TRUE;
    }
    if (g_map_state == -1) return TRUE;
    /* Otherwise behave exactly like the main window, so playing works
     * whichever window has focus: a system action first, on its leading
     * edge only (GTK4 has no repeat flag). */
    sa = studio2session_key_sysaction(g_session, keysym);
    if (sa >= 0) {
        if (!g_sysact_down[sa]) {
            g_sysact_down[sa] = TRUE;
            run_sysaction(sa);
        }
        return TRUE;
    }
    return studio2session_key(g_session, keysym, 1) ? TRUE : FALSE;
}

static gboolean on_key_released(GtkEventControllerKey *c, guint keyval,
                                guint code, GdkModifierType st, gpointer d)
{
    guint32 keysym = code >= 8 ? studio2session_keysym_from_evdev(code - 8) : 0;
    int sa;
    (void)c; (void)st; (void)d;
    if (!keysym) keysym = keyval;
    if (g_map_state != -2) return TRUE;
    sa = studio2session_key_sysaction(g_session, keysym);
    if (sa >= 0) {
        g_sysact_down[sa] = FALSE;
        return TRUE;
    }
    return studio2session_key(g_session, keysym, 0) ? TRUE : FALSE;
}

/* Keys and mouse buttons held into a focus change must not stay down. */
static void on_focus_leave(GtkEventControllerFocus *c, gpointer d)
{
    (void)c; (void)d;
    studio2session_release_all(g_session);
    memset(g_sysact_down, 0, sizeof g_sysact_down);
    release_pointer();
}

static void on_active_changed(GObject *o, GParamSpec *ps, gpointer d)
{
    (void)ps; (void)d;
    if (!gtk_window_is_active(GTK_WINDOW(o)))
        release_pointer();
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_clear_clicked(GtkButton *b, gpointer d)
{
    char msg[160];
    (void)b; (void)d;
    if (g_map_state < 0) return;
    studio2session_binding_clear(g_session, g_map_state);
    g_snprintf(msg, sizeof msg, "Cleared %s", studio2_target_name(g_map_state));
    set_hint(msg);
    gtk_widget_queue_draw(g_area);
}

static void on_defaults_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    studio2session_bindings_reset(g_session);
    set_hint("Back to the defaults");
    gtk_widget_queue_draw(g_area);
}

static void on_show_toggled(GtkCheckButton *b, gpointer d)
{
    (void)d;
    g_show_bindings = gtk_check_button_get_active(b);
    studio2session_set_int(g_session, "controllers_show_bindings", g_show_bindings);
    gtk_widget_queue_draw(g_area);
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)d;
    set_map_state(-2);
    release_pointer();
    gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
    return TRUE;
}

static void on_window_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    release_pointer();
    if (!g_held_timer)
        g_held_timer = g_timeout_add(33, held_tick, NULL);
    if (!g_pad_timer)
        g_pad_timer = g_timeout_add(250, pad_tick, NULL);
}

static void on_window_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    release_pointer();
    if (g_held_timer) { g_source_remove(g_held_timer); g_held_timer = 0; }
    if (g_pad_timer) { g_source_remove(g_pad_timer); g_pad_timer = 0; }
}

static GtkWidget *plain_button(const char *label, GCallback cb)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_set_focus_on_click(b, FALSE);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *maprow, *toolbar, *header, *padtitle;
    GtkEventController *keys, *focus;
    GtkGesture *click;
    float lw, lh;

    g_parent = parent;
    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
    /* Same app-id as the main window, so the shell groups and icons it. */
    gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "notify::is-active", G_CALLBACK(on_active_changed), NULL);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    studio2session_controls_layout(NULL, &lw, &lh);
    g_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_area), (int)(lw * SCALE + 2 * MARGIN));
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_area), (int)(lh * SCALE + 2 * MARGIN));
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_area), draw_console, NULL, NULL);
    gtk_widget_set_halign(g_area, GTK_ALIGN_CENTER);
    gtk_widget_set_focusable(g_area, FALSE);
    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), NULL);
    g_signal_connect(click, "released", G_CALLBACK(on_released), NULL);
    g_signal_connect(click, "cancel", G_CALLBACK(on_cancelled), NULL);
    gtk_widget_add_controller(g_area, GTK_EVENT_CONTROLLER(click));
    gtk_box_append(GTK_BOX(root), g_area);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = plain_button("Map", G_CALLBACK(on_map_clicked));
    g_clear_button = plain_button("Clear", G_CALLBACK(on_clear_clicked));
    g_defaults_button = plain_button("Defaults", G_CALLBACK(on_defaults_clicked));
    g_show_check = gtk_check_button_new_with_label("Show bindings");
    g_show_bindings = studio2session_get_int(g_session, "controllers_show_bindings", 0) != 0;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_show_check), g_show_bindings);
    gtk_widget_set_focus_on_click(g_show_check, FALSE);
    g_signal_connect(g_show_check, "toggled", G_CALLBACK(on_show_toggled), NULL);
    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    /* a long hint ("Press a key or gamepad button for Keypad B: Key 7")
     * must not widen the window */
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(g_map_hint), 12);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 24);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), g_clear_button);
    gtk_box_append(GTK_BOX(maprow), g_defaults_button);
    gtk_box_append(GTK_BOX(maprow), g_show_check);
    gtk_box_append(GTK_BOX(maprow), g_map_hint);
    gtk_box_append(GTK_BOX(root), maprow);

    padtitle = gtk_label_new("Gamepads");
    gtk_widget_add_css_class(padtitle, "heading");
    gtk_label_set_xalign(GTK_LABEL(padtitle), 0);
    gtk_box_append(GTK_BOX(root), padtitle);
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(root), g_pad_box);

    header = adw_header_bar_new();
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), studio2_help_button("controllers"));
    studio2_help_set_context(GTK_WINDOW(g_window), "controllers");
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);
    set_map_state(-2);
}

void studio2_controllers_show(GtkWindow *parent, studio2session *session)
{
    g_session = session;
    if (!g_window)
        build_window(parent);
    g_pad_generation = (unsigned)-1;
    pad_tick(NULL);
    gtk_window_present(GTK_WINDOW(g_window));
    pad_tick(NULL);
}

void studio2_controllers_toggle(GtkWindow *parent, studio2session *session)
{
    g_session = session;
    if (!g_window)
        build_window(parent);

    if (gtk_widget_get_visible(g_window)) {
        set_map_state(-2);
        release_pointer();
        gtk_widget_set_visible(g_window, FALSE);
    } else {
        studio2_controllers_show(parent, session);
    }
}

gboolean studio2_controllers_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}
