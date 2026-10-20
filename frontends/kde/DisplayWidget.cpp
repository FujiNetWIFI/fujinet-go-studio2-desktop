/*
 * DisplayWidget -- see DisplayWidget.h.
 *
 * A QTimer rather than a frame-clock callback: Qt Widgets has no equivalent
 * of GdkFrameClock. The timer runs a little faster than the machine so no
 * frame waits a whole period, and the session's own wall-clock pacing does
 * the real work -- notify_vsync is still fed, so the phase lock engages
 * whenever the ticks happen to be steady.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include <QPainter>
#include <chrono>
#include <cstring>

DisplayWidget::DisplayWidget(studio2session *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    m_fb.resize(STUDIO2SESSION_FB_WIDTH * STUDIO2SESSION_FB_HEIGHT);
    m_image = QImage(STUDIO2SESSION_FB_WIDTH, STUDIO2SESSION_FB_HEIGHT, QImage::Format_RGB32);
    /* the 246 lines once, at the TV's 4:3 */
    setMinimumSize(328, 246);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);

    connect(&m_timer, &QTimer::timeout, this, &DisplayWidget::tick);
    /* ~120 Hz: comfortably above the machine's 60 so a finished frame is
     * never held back by the poll interval. */
    m_timer.start(8);
}

void DisplayWidget::tick()
{
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    studio2session_notify_vsync(
        m_session,
        (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());

    if (!studio2session_copy_frame(m_session, m_fb.data(), &m_serial))
        return;

    /* The session's pixels are 0x00RRGGBB, which is exactly
     * QImage::Format_RGB32 (the alpha byte is ignored); a straight copy. */
    std::memcpy(m_image.bits(), m_fb.data(),
                (size_t)STUDIO2SESSION_FB_WIDTH * STUDIO2SESSION_FB_HEIGHT * 4);
    m_haveFrame = true;
    update();
}

void DisplayWidget::setTvAspect(bool tv) { m_tvAspect = tv; update(); }
void DisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }

void DisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (!m_haveFrame) return;

    /* The 1861 paints 100 x 246 (its 64 x 128 picture inside the blanking
     * MAME keeps) across the whole of a 4:3 television, so on the TV a
     * Studio II pixel is more than three times as wide as it is tall.
     * Square pixels show the raw buffer: a very tall, narrow picture. */
    const double want = m_tvAspect ? 4.0 / 3.0
                                   : (double)STUDIO2SESSION_FB_WIDTH / (double)STUDIO2SESSION_FB_HEIGHT;
    double w = width(), h = height(), sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }

    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    p.drawImage(QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh), m_image);
}
