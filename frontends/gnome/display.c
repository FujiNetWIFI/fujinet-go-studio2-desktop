/*
 * display.c -- see display.h.
 *
 * Two things here are load bearing and neither is obvious:
 *
 *  - The frame clock is the emulator's clock. GTK4 presents on the
 *    compositor's vsync, so its tick callback is the most accurate ~60 Hz
 *    signal available, and feeding it to the session lets the emulator run
 *    one frame per tick instead of racing its own timer against the panel's.
 *    (The Studio II runs at 59.98 Hz, near enough a 60 Hz panel to lock;
 *    on a panel far from that the session paces it on the wall clock.)
 *
 *  - Frames are pulled by serial, not pushed. copy_frame does nothing when
 *    the emulator has not produced a new frame, so a tick that arrives
 *    between frames costs a mutex and no memcpy.
 *
 * The CDP1861's frame is always 100 x 246 (MAME's visible area: the 64 x 128
 * picture inside the blanking the chip draws nothing into), white on black,
 * so the texture is the same size every time.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"

#include <string.h>

#define FB_W STUDIO2SESSION_FB_WIDTH
#define FB_H STUDIO2SESSION_FB_HEIGHT

/* A television showed the whole visible area at 4:3: each of the 1861's
 * dots about three and a third lines wide. */
#define TV_ASPECT (4.0 / 3.0)

struct _Studio2Display {
    GtkWidget parent_instance;

    studio2session *session;
    guint32 *fb;           /* XRGB8888 straight from the session */
    guint32 *bgra;         /* with alpha forced opaque for GdkMemoryTexture */
    GdkTexture *texture;
    guint64 serial;
    guint tick_id;
    gboolean tv_aspect;
    gboolean smooth;
};

G_DEFINE_FINAL_TYPE(Studio2Display, studio2_display, GTK_TYPE_WIDGET)

static void rebuild_texture(Studio2Display *self)
{
    GBytes *bytes;
    int i, n = FB_W * FB_H;

    /* The session's pixels are 0x00RRGGBB; GDK_MEMORY_B8G8R8A8 on a little-endian
     * host reads the same bytes as B,G,R,A -- only the alpha needs setting. */
    for (i = 0; i < n; i++)
        self->bgra[i] = self->fb[i] | 0xFF000000u;

    bytes = g_bytes_new_static(self->bgra, (gsize)n * 4);
    g_clear_object(&self->texture);
    self->texture = gdk_memory_texture_new(FB_W, FB_H, GDK_MEMORY_B8G8R8A8, bytes,
                                           (gsize)FB_W * 4);
    g_bytes_unref(bytes);
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    Studio2Display *self = STUDIO2_DISPLAY(widget);
    (void)user_data;

    /* Hand the emulator the compositor's cadence. */
    studio2session_notify_vsync(self->session,
                            gdk_frame_clock_get_frame_time(clock) * 1000);

    if (studio2session_copy_frame(self->session, self->fb, &self->serial)) {
        rebuild_texture(self);
        gtk_widget_queue_draw(widget);
    }
    return G_SOURCE_CONTINUE;
}

static void studio2_display_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    Studio2Display *self = STUDIO2_DISPLAY(widget);
    int w = gtk_widget_get_width(widget);
    int h = gtk_widget_get_height(widget);
    double want, sw, sh, x, y;
    graphene_rect_t rect;

    gtk_snapshot_append_color(snapshot, &(GdkRGBA){ 0, 0, 0, 1 },
                              &GRAPHENE_RECT_INIT(0, 0, w, h));
    if (!self->texture || w <= 0 || h <= 0)
        return;

    /* TV mode is the picture as a television showed it: the whole 100 x
     * 246 area filling a 4:3 screen, so the 1861's 64 x 128 pixels are the
     * familiar wide blocks. Square pixels show the raw buffer -- a very tall
     * 100:246 strip -- which is useful for pixel work and honest about
     * nothing else. */
    want = self->tv_aspect ? TV_ASPECT : (double)FB_W / (double)FB_H;

    if ((double)w / (double)h > want) {
        sh = h;
        sw = sh * want;
    } else {
        sw = w;
        sh = sw / want;
    }
    x = (w - sw) / 2.0;
    y = (h - sh) / 2.0;

    graphene_rect_init(&rect, (float)x, (float)y, (float)sw, (float)sh);
    gtk_snapshot_append_scaled_texture(
        snapshot, self->texture,
        self->smooth ? GSK_SCALING_FILTER_LINEAR : GSK_SCALING_FILTER_NEAREST,
        &rect);
}

static void studio2_display_dispose(GObject *object)
{
    Studio2Display *self = STUDIO2_DISPLAY(object);

    if (self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    g_clear_object(&self->texture);
    g_clear_pointer(&self->fb, g_free);
    g_clear_pointer(&self->bgra, g_free);

    G_OBJECT_CLASS(studio2_display_parent_class)->dispose(object);
}

static void studio2_display_class_init(Studio2DisplayClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = studio2_display_dispose;
    GTK_WIDGET_CLASS(klass)->snapshot = studio2_display_snapshot;
}

static void studio2_display_init(Studio2Display *self)
{
    self->fb = g_new0(guint32, FB_W * FB_H);
    self->bgra = g_new0(guint32, FB_W * FB_H);
    self->tv_aspect = TRUE;
    self->smooth = FALSE;
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
}

GtkWidget *studio2_display_new(studio2session *session)
{
    Studio2Display *self = g_object_new(STUDIO2_TYPE_DISPLAY, NULL);
    self->session = session;
    self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), on_tick,
                                                 NULL, NULL);
    return GTK_WIDGET(self);
}

void studio2_display_set_tv_aspect(Studio2Display *self, gboolean tv)
{
    self->tv_aspect = tv;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void studio2_display_set_smooth(Studio2Display *self, gboolean smooth)
{
    self->smooth = smooth;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
