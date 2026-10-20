/*
 * The emulator display: a QWidget that pulls frames from the session.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QTimer>
#include <QWidget>
#include <cstdint>
#include <vector>

#include "studio2session.h"

class DisplayWidget : public QWidget {
    Q_OBJECT
public:
    explicit DisplayWidget(studio2session *session, QWidget *parent = nullptr);
    /* "aspect" setting: 0 = the picture at the TV's 4:3, 1 = square pixels
     * (a very tall 100:246 picture); this takes "TV aspect on". */
    void setTvAspect(bool tv);
    void setSmooth(bool smooth);

protected:
    void paintEvent(QPaintEvent *) override;
    /* Tab is a key like any other here (it can be bound to a control): it
     * reaches the window's key handler instead of moving the focus. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void tick();

    studio2session *m_session;
    QImage m_image;
    std::vector<uint32_t> m_fb;
    bool m_haveFrame = false;
    uint64_t m_serial = 0;
    QTimer m_timer;
    bool m_tvAspect = true;
    bool m_smooth = false;
};
