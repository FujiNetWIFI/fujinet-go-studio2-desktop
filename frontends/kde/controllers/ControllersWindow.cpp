/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * The RCA Studio II as a player sees it from above: a long, low tan case with
 * a recessed well in each wing -- a black bezel round an aluminium plate with
 * ten black keys, 1 2 3 / 4 5 6 / 7 8 9 and the 0 centred under the 8, A on
 * the left and B on the right -- and between them the centre panel: a gold
 * plate with the RCA badge, the script "Studio II" and HOME TV PROGRAMMER, a
 * ribbed panel below it, and on that a silver plate holding the red LED and
 * the black CLEAR bar. The two buttons that are not on the real console
 * (CONFIG, STOP) sit in the ribbed area. Everything is painted from the
 * session's layout (studio2session_controls_layout, in key units), so every
 * frontend shows the same parts in the same places; the colours are this
 * file's, read off photographs of the real hardware (DESIGN-studio2.md).
 *
 * Gestures act on mouse press AND release: the console scans its keypads once
 * a frame, so a press present only for the instant of a click would fall
 * between frames. A key or CLEAR is held while the mouse is down on it
 * (dragging off lets it go). Holding CLEAR holds the CPU in reset, so
 * everything the mouse holds is let go of on release, on losing the focus,
 * on hiding the window and on entering Map.
 *
 * What the console sees held -- host keyboard, gamepads and this window -- is
 * lit in the accent (tan, with black ink), so the window doubles as an input
 * tester for a new pad. The LED is lit while CLEAR is held. Keys typed with
 * the window active go to the console as in the main window.
 *
 * FIXED SIZE: the drawing is a fixed number of pixels per layout unit, Map
 * legends are fitted into their parts, and gamepad names are elided, so
 * neither Map mode nor a long name resizes the window. A singleton, hidden
 * rather than destroyed, so a half-done remap survives closing it. The timers
 * run only while it is visible.
 *
 * MAP MODE: Map arms a two-step rebind -- click a part (a key, CLEAR, CONFIG
 * or STOP), then press a host key or a gamepad button. Rebinding steals the
 * key from whatever held it (named in the hint). Escape cancels; while Map is
 * armed nothing is typed into the console. F9, F11 and F12 cannot be bound.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QRadialGradient>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QtMath>

#include <functional>
#include <utility>
#include <vector>

#include "../FujiNetWindows.h"
#include "../KeyForward.h"
#include "../debugger/DebuggerWindow.h"
#include "../help/HelpWindow.h"

namespace {

using studio2ctl::kCasePad;
using studio2ctl::kTop;
using studio2ctl::kUnit;

constexpr int kMaxPadRows = 3;
constexpr qreal kDimAlpha = 0.4;      /* an unpicked part while Map waits for a key */

/* The console (photo-read or inferred, see DESIGN-studio2.md). */
const QColor kCaseTop(0xC8, 0xB5, 0x8C);
const QColor kCaseLight(0xD4, 0xC4, 0x9E);
const QColor kCaseEdge(0xA8, 0x94, 0x6A);
const QColor kCaseBand(0x6E, 0x66, 0x48);
const QColor kBezel(0x1C, 0x1A, 0x16);
const QColor kAluminium(0xC0, 0xC1, 0xBB);
const QColor kBrush(0xB0, 0xB1, 0xAB);
const QColor kMark(0x62, 0x63, 0x5E);          /* the direction triangles */
const QColor kKeyFace(0x2A, 0x28, 0x22);
const QColor kKeyBase(0x12, 0x11, 0x0E);
const QColor kKeyHover(0x3E, 0x3B, 0x33);
const QColor kKeyOutline(0x7A, 0x66, 0x52);
const QColor kLegend(0xF2, 0xF2, 0xEC);
const QColor kGold(0xC4, 0xB5, 0x8C);
const QColor kGoldEdge(0x9A, 0x8C, 0x66);
const QColor kRibbed(0xB3, 0xAC, 0x7C);
const QColor kRibGroove(0x98, 0x91, 0x66);
const QColor kSilver(0xCD, 0xCE, 0xC1);
const QColor kTrim(0xDA, 0xDB, 0xD3);
const QColor kInk(0x1A, 0x18, 0x14);
const QColor kLedOff(0x8E, 0x1E, 0x1A);
const QColor kLedOn(0xE8, 0x37, 0x2E);
const QColor kBadge(0xD6, 0xC8, 0xA0);

QString local(const char *text) { return QString::fromUtf8(text ? text : ""); }

QFont pixelFont(QFont fnt, int pixels, bool bold)
{
    fnt.setPixelSize(pixels);
    fnt.setBold(bold);
    return fnt;
}

/* Lines centred in rect, the font shrunk until they fit. */
void drawFitted(QPainter &pnt, const QRectF &rect, const QString &text, QFont fnt)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (;;) {
        const QFontMetricsF metrics(fnt);
        qreal wide = 0;
        for (const QString &line : lines) wide = qMax(wide, metrics.horizontalAdvance(line));
        const qreal size = fnt.pixelSize() > 0 ? fnt.pixelSize() : fnt.pointSizeF();
        if ((wide <= rect.width() && metrics.lineSpacing() * lines.size() <= rect.height() + 2) || size <= 5.0)
            break;
        if (fnt.pixelSize() > 0) fnt.setPixelSize(fnt.pixelSize() - 1);
        else fnt.setPointSizeF(fnt.pointSizeF() - 0.5);
    }
    pnt.setFont(fnt);
    pnt.drawText(rect, Qt::AlignCenter, text);
}

/* A target's binding as Map and Show bindings write it ("Q / A"), or empty. */
QString bindingRaw(studio2session *s, int target)
{
    char text[96] = "";
    studio2session_binding_text(s, target, text, sizeof text);
    return local(text);
}

/* Long key names shortened to fit a key (tooltips keep the full name). */
QString shorten(QString text)
{
    text.replace(QStringLiteral("Keypad "), QStringLiteral("KP "));
    text.replace(QStringLiteral("Left "), QStringLiteral("L "));
    text.replace(QStringLiteral("Right "), QStringLiteral("R "));
    text.replace(QStringLiteral("Backspace"), QStringLiteral("BkSp"));
    text.replace(QStringLiteral("Escape"), QStringLiteral("Esc"));
    text.replace(QStringLiteral("Return"), QStringLiteral("Ret"));
    return text;
}

void triangle(QPainter &pnt, const QPointF &mid, qreal size, qreal dx, qreal dy, const QColor &ink)
{
    /* points along (dx, dy) */
    const QPointF dir(dx, dy), side(-dy, dx);
    QPolygonF poly;
    poly << mid + dir * size << mid - dir * size * 0.6 + side * size * 0.8
         << mid - dir * size * 0.6 - side * size * 0.8;
    pnt.setPen(Qt::NoPen);
    pnt.setBrush(ink);
    pnt.drawPolygon(poly);
}

/* ---- ElidedLabel ------------------------------------------------------------ */

/* A label that never widens the window: one line elides, more wrap into a
 * fixed height. */
class ElidedLabel : public QWidget {
public:
    explicit ElidedLabel(bool dim, int lines = 1) : m_dim(dim), m_lines(lines)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setText(const QString &text)
    {
        if (text == m_text) return;
        m_text = text;
        update();
    }
    QString text() const { return m_text; }
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * m_lines); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setPen(m_dim ? QColor(Qt::gray) : palette().color(QPalette::WindowText));
        if (m_lines > 1)
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        else
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
    }

private:
    QString m_text;
    bool m_dim;
    int m_lines;
};

/* ---- ConsoleWidget: the console top ------------------------------------------ */

class ConsoleWidget : public QWidget {
public:
    /* onPick: a part was chosen in Map mode (its target); onHint: a message for the hint line */
    ConsoleWidget(studio2session *session, std::function<void(int)> onPick)
        : m_session(session), m_onPick(std::move(onPick))
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        load();
    }

    /* Lets go of everything the mouse holds (also on focus loss, Map entry
     * and hide): a CLEAR left down holds the CPU in reset. */
    void releaseAll()
    {
        studio2session_release_pointer(m_session);
        m_hold = None;
        m_holdIdx = -1;
        update();
    }

    /* -2 idle, -1 armed, >= 0 waiting for a key or pad button for that target */
    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void setShowBindings(bool visible)
    {
        m_showBindings = visible;
        update();
    }

    /* What the console sees held (keyboard, gamepads, the mouse); repaints
     * only on a change. */
    void pollHeld()
    {
        bool changed = false;
        for (int idx = 0; idx < m_count; ++idx) {
            const studio2_control &ctl = m_layout[idx];
            if (ctl.kind != STUDIO2_CTL_KEY && ctl.kind != STUDIO2_CTL_CLEAR) continue;
            const char held = studio2session_target_held(m_session, ctl.target) ? 1 : 0;
            if (held != m_held[(size_t)idx]) {
                m_held[(size_t)idx] = held;
                changed = true;
            }
        }
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        paintCase(pnt);
        for (int idx = 0; idx < m_count; ++idx) {
            const studio2_control &ctl = m_layout[idx];
            switch (ctl.kind) {
            case STUDIO2_CTL_WELL: paintWell(pnt, idx); break;
            case STUDIO2_CTL_CAPTION: paintCaption(pnt, idx); break;
            case STUDIO2_CTL_PANEL: paintPanel(pnt, idx); break;
            case STUDIO2_CTL_LED: paintLed(pnt, idx); break;
            case STUDIO2_CTL_KEY:
            case STUDIO2_CTL_CLEAR: paintKey(pnt, idx); break;
            case STUDIO2_CTL_SYSBUTTON: paintSys(pnt, idx); break;
            default: break;
            }
        }
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        const QPointF pos = evt->position();
        const int idx = hitAt(pos);
        if (m_map != -2) {
            if (idx >= 0) m_onPick(m_layout[idx].target);
            return;
        }
        if (m_hold != None || idx < 0) return;     /* one thing at a time */
        const studio2_control &ctl = m_layout[idx];
        m_holdIdx = idx;
        m_partDown = true;
        if (ctl.kind == STUDIO2_CTL_SYSBUTTON) {
            m_hold = Strip;      /* fires on release */
        } else {
            m_hold = Part;
            studio2session_pointer_press(m_session, ctl.target, 1);
            pollHeld();
        }
        update();
    }

    /* Dragging off a part lets it go (and back on, holds it again). */
    void mouseMoveEvent(QMouseEvent *evt) override
    {
        const QPointF pos = evt->position();
        const int hover = m_hold == None ? hitAt(pos) : -1;
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
        if (m_hold == Part || m_hold == Strip) {
            const bool over = hitAt(pos) == m_holdIdx;
            if (over != m_partDown) {
                m_partDown = over;
                if (m_hold == Part)
                    studio2session_pointer_press(m_session, m_layout[m_holdIdx].target, over ? 1 : 0);
                pollHeld();
                update();
            }
        }
    }

    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        if (m_hold == Strip && hitAt(evt->position()) == m_holdIdx) {
            /* fires on release, like a real button: dragging off cancels. The
             * main window's queue runs it, as it does for a key or a pad. */
            studio2session_sysaction_post(m_session,
                                          m_layout[m_holdIdx].target - STUDIO2_TARGET_SYSACT(0));
        }
        if (m_hold != None) releaseAll();
        pollHeld();
        m_hover = hitAt(evt->position());
        update();
    }

    void leaveEvent(QEvent *evt) override
    {
        if (m_hover != -1) {
            m_hover = -1;
            update();
        }
        QWidget::leaveEvent(evt);
    }

    bool event(QEvent *evt) override
    {
        if (evt->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(evt);
            const QString tip = tipAt(help->pos());
            if (tip.isEmpty())
                QToolTip::hideText();
            else
                QToolTip::showText(help->globalPos(), tip, this);
            return true;
        }
        return QWidget::event(evt);
    }

private:
    enum Hold { None, Part, Strip };

    void load()
    {
        float wide = 0, tall = 0;
        m_layout = studio2session_controls_layout(&m_count, &wide, &tall);
        m_held.assign((size_t)m_count, 0);
        for (auto &row : m_key)
            for (int &slot : row) slot = -1;
        for (int idx = 0; idx < m_count; ++idx) {
            const studio2_control &ctl = m_layout[idx];
            if (ctl.kind == STUDIO2_CTL_KEY && ctl.port >= 0 && ctl.port < 2) {
                const int key = ctl.target % STUDIO2_ACT_PER_PORT;
                if (key >= 0 && key < 10) m_key[ctl.port][key] = idx;
            } else if (ctl.kind == STUDIO2_CTL_PANEL) {
                const QString name = local(ctl.label);
                const QRectF box = rectOf(idx);
                if (name == QLatin1String("gold")) m_gold = box;
                else if (name == QLatin1String("ribbed")) m_ribbed = box;
                else if (name == QLatin1String("silver")) m_silver = box;
            }
        }
        m_panelW = qCeil(wide * kUnit) + 2 * kCasePad;
        m_panelH = kTop + qCeil(tall * kUnit) + kCasePad;
        setFixedSize(m_panelW, m_panelH);
    }

    QRectF rectOf(int idx) const
    {
        const studio2_control &ctl = m_layout[idx];
        return QRectF(kCasePad + ctl.xpos * kUnit, kTop + ctl.ypos * kUnit, ctl.width * kUnit, ctl.height * kUnit);
    }

    int hitAt(const QPointF &pos) const
    {
        return studio2session_controls_hit(float((pos.x() - kCasePad) / kUnit), float((pos.y() - kTop) / kUnit));
    }

    QString tipAt(const QPoint &pos) const
    {
        const int idx = hitAt(pos);
        if (idx < 0) return QString();
        const int target = m_layout[idx].target;
        const QString bind = bindingRaw(m_session, target);
        return QStringLiteral("%1\nBound to: %2")
            .arg(local(studio2_target_name(target)), bind.isEmpty() ? QStringLiteral("nothing") : bind);
    }

    bool mapping() const { return m_map != -2; }
    bool showBind() const { return mapping() || m_showBindings; }

    /* A part's lit state: held by anyone, or the one Map picked. */
    bool lit(int idx) const
    {
        const studio2_control &ctl = m_layout[idx];
        if (mapping()) return ctl.target >= 0 && ctl.target == m_map;
        if (ctl.kind == STUDIO2_CTL_SYSBUTTON) return m_hold == Strip && m_holdIdx == idx && m_partDown;
        return m_held[(size_t)idx] != 0;
    }

    /* While Map waits for a key, every part but the picked one steps back. */
    bool dimmed(const studio2_control &ctl) const { return m_map >= 0 && ctl.target != m_map; }

    /* ---- painting ---- */

    void paintCase(QPainter &pnt)
    {
        const QRectF body(0.5, 0.5, width() - 1.0, height() - 1.0);
        QPainterPath shape;
        shape.addRoundedRect(body, 18, 18);
        QLinearGradient tan(0, 0, 0, height());
        tan.setColorAt(0, kCaseLight);
        tan.setColorAt(0.35, kCaseTop);
        tan.setColorAt(1, QColor(0xBC, 0xA8, 0x7E));
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(tan);
        pnt.drawPath(shape);
        /* the dark underside band, seen along the bottom edge */
        pnt.save();
        pnt.setClipPath(shape);
        pnt.setBrush(kCaseBand);
        pnt.drawRect(QRectF(0, height() - 7, width(), 8));
        pnt.restore();
        pnt.setPen(QPen(kCaseEdge, 1));
        pnt.setBrush(Qt::NoBrush);
        pnt.drawPath(shape);
        /* the bevel where the top face meets the sides */
        pnt.setPen(QPen(kCaseEdge, 1));
        pnt.drawRoundedRect(QRectF(5.5, 5.5, width() - 11.0, height() - 18.0), 13, 13);
    }

    void paintWell(QPainter &pnt, int idx)
    {
        const QRectF box = rectOf(idx);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kBezel);
        pnt.drawRoundedRect(box, 5, 5);
        const QRectF plate = box.adjusted(5, 5, -5, -5);
        pnt.setBrush(kAluminium);
        pnt.drawRoundedRect(plate, 2, 2);
        pnt.setPen(QPen(kBrush, 1));
        for (qreal y = plate.top() + 2; y < plate.bottom(); y += 3)
            pnt.drawLine(QPointF(plate.left() + 1, y), QPointF(plate.right() - 1, y));

        /* the direction marks round the grid: 2 4 6 8 and the diagonals */
        const int port = m_layout[idx].port;
        if (port < 0 || port > 1) return;
        auto keyRect = [&](int key) { return m_key[port][key] >= 0 ? rectOf(m_key[port][key]) : QRectF(); };
        const QRectF k1 = keyRect(1), k2 = keyRect(2), k3 = keyRect(3), k4 = keyRect(4), k6 = keyRect(6),
                     k7 = keyRect(7), k8 = keyRect(8), k9 = keyRect(9), k0 = keyRect(0);
        if (k1.isEmpty() || k9.isEmpty() || k0.isEmpty()) return;
        const qreal sz = 3.4;
        triangle(pnt, QPointF(k2.center().x(), k2.top() - 7), sz, 0, -1, kMark);
        triangle(pnt, QPointF(k4.left() - 7, k4.center().y()), sz, -1, 0, kMark);
        triangle(pnt, QPointF(k6.right() + 7, k6.center().y()), sz, 1, 0, kMark);
        triangle(pnt, QPointF(k8.center().x(), (k8.bottom() + k0.top()) / 2), sz, 0, 1, kMark);
        const qreal d = 0.7071;
        triangle(pnt, QPointF(k1.left() - 7, k1.top() - 7), sz * 0.85, -d, -d, kMark);
        triangle(pnt, QPointF(k3.right() + 7, k3.top() - 7), sz * 0.85, d, -d, kMark);
        triangle(pnt, QPointF(k7.left() - 7, k7.bottom() + 7), sz * 0.85, -d, d, kMark);
        triangle(pnt, QPointF(k9.right() + 7, k9.bottom() + 7), sz * 0.85, d, d, kMark);
    }

    void paintCaption(QPainter &pnt, int idx)
    {
        const studio2_control &ctl = m_layout[idx];
        const QString text = local(ctl.label);
        const QRectF box = rectOf(idx);
        if (ctl.port >= 0) {          /* the raised A and B above the wells */
            pnt.setFont(pixelFont(font(), 16, true));
            pnt.setPen(QColor(0x8F, 0x7E, 0x58));
            pnt.drawText(box.translated(1, 1), Qt::AlignCenter, text);
            pnt.setPen(QColor(0xEE, 0xE6, 0xCE));
            pnt.drawText(box, Qt::AlignCenter, text);
        } else if (text == QLatin1String("RCA")) {
            pnt.setPen(QPen(kGoldEdge, 1));
            pnt.setBrush(kBadge);
            pnt.drawRect(box);
            pnt.setPen(QColor(0xFB, 0xF8, 0xEC));
            QFont fnt = pixelFont(font(), 13, true);
            fnt.setItalic(true);
            drawFitted(pnt, box.adjusted(1, 0, -1, 0), text, fnt);
        } else if (text == QLatin1String("Studio II")) {
            QFont fnt(QStringLiteral("serif"));
            fnt.setItalic(true);
            fnt.setPixelSize(32);
            pnt.setPen(kInk);
            drawFitted(pnt, box, text, fnt);
        } else {                       /* HOME TV PROGRAMMER, the CLEAR legend */
            pnt.setPen(kInk);
            QFont fnt = pixelFont(font(), text == QLatin1String("CLEAR") ? 9 : 10, false);
            fnt.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
            drawFitted(pnt, box, text, fnt);
        }
    }

    void paintPanel(QPainter &pnt, int idx)
    {
        const QString name = local(m_layout[idx].label);
        const QRectF box = rectOf(idx);
        pnt.setBrush(Qt::NoBrush);
        if (name == QLatin1String("gold")) {
            /* the chrome trim round the whole centre panel */
            const QRectF all = m_gold.united(m_ribbed).adjusted(-4, -4, 4, 4);
            pnt.setPen(QPen(QColor(0x9C, 0x9D, 0x96), 1));
            pnt.setBrush(kTrim);
            pnt.drawRoundedRect(all, 4, 4);
            QLinearGradient gold(box.topLeft(), box.bottomLeft());
            gold.setColorAt(0, QColor(0xCB, 0xBD, 0x95));
            gold.setColorAt(1, kGold);
            pnt.setPen(QPen(kGoldEdge, 1));
            pnt.setBrush(gold);
            pnt.drawRect(box);
        } else if (name == QLatin1String("ribbed")) {
            pnt.setPen(QPen(kGoldEdge, 1));
            pnt.setBrush(kRibbed);
            pnt.drawRect(box);
            pnt.setPen(QPen(kRibGroove, 1));
            for (qreal x = box.left() + 3; x < box.right() - 1; x += 4)
                pnt.drawLine(QPointF(x, box.top() + 2), QPointF(x, box.bottom() - 2));
        } else {
            pnt.setPen(QPen(QColor(0x9C, 0x9D, 0x96), 1));
            pnt.setBrush(kSilver);
            pnt.drawRect(box);
        }
    }

    void paintLed(QPainter &pnt, int idx)
    {
        const QRectF box = rectOf(idx);
        const bool on = studio2session_target_held(m_session, m_layout[idx].target) != 0;
        const QPointF mid = box.center();
        const qreal rad = box.width() / 2;
        QRadialGradient chrome(mid - QPointF(2, 2), rad * 1.3);
        chrome.setColorAt(0, QColor(0xF4, 0xF4, 0xEE));
        chrome.setColorAt(1, QColor(0x8C, 0x8D, 0x87));
        pnt.setPen(QPen(QColor(0x6A, 0x6B, 0x66), 1));
        pnt.setBrush(chrome);
        pnt.drawEllipse(mid, rad, rad);
        if (on) {
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(QColor(0xE8, 0x37, 0x2E, 70));
            pnt.drawEllipse(mid, rad * 1.5, rad * 1.5);
        }
        QRadialGradient lens(mid - QPointF(1.5, 1.5), rad * 0.8);
        lens.setColorAt(0, on ? QColor(0xFF, 0x9A, 0x8A) : QColor(0xB8, 0x4A, 0x40));
        lens.setColorAt(1, on ? kLedOn : kLedOff);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(lens);
        pnt.drawEllipse(mid, rad * 0.62, rad * 0.62);
    }

    /* A keypad key or the CLEAR bar: a black key with a darker base; held, it
     * sits lower and wears the accent with black ink. */
    void paintKey(QPainter &pnt, int idx)
    {
        const studio2_control &ctl = m_layout[idx];
        const bool on = lit(idx);
        const bool over = !on && m_hover == idx && m_hold == None;
        const QRectF full = rectOf(idx);
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kKeyBase);
        pnt.drawRoundedRect(full, 4, 4);
        const QRectF face = on ? full.adjusted(0, 2, 0, 0) : full.adjusted(0, 0, 0, -3);
        if (on) {
            pnt.setBrush(studio2AccentColor());
            pnt.setPen(QPen(kKeyOutline, 1.5));
        } else {
            QLinearGradient grad(face.topLeft(), face.bottomLeft());
            grad.setColorAt(0, over ? kKeyHover.lighter(115) : kKeyHover.darker(over ? 100 : 105));
            grad.setColorAt(1, over ? kKeyHover : kKeyFace);
            pnt.setBrush(grad);
            pnt.setPen(QPen(kKeyBase, 1));
        }
        pnt.drawRoundedRect(face, 4, 4);
        pnt.setPen(on ? QColor(Qt::black) : kLegend);
        /* the real CLEAR bar is blank (its word is printed under it) */
        QString text = ctl.kind == STUDIO2_CTL_CLEAR ? QString() : local(ctl.label);
        if (showBind()) {
            const QString bind = shorten(bindingRaw(m_session, ctl.target));
            text = bind.isEmpty() ? QStringLiteral("—") : bind;
            text.replace(QStringLiteral(" / "), QStringLiteral("\n"));
        }
        const bool wide = ctl.kind == STUDIO2_CTL_CLEAR;
        drawFitted(pnt, face.adjusted(2, 0, -2, 0), text,
                   pixelFont(font(), showBind() ? 10 : (wide ? 10 : 12), !showBind() && !wide));
        pnt.setOpacity(1.0);
    }

    void paintSys(QPainter &pnt, int idx)
    {
        const studio2_control &ctl = m_layout[idx];
        const bool on = lit(idx);
        const bool over = !on && m_hover == idx && m_hold == None;
        const QRectF full = rectOf(idx);
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kKeyBase);
        pnt.drawRoundedRect(full, 5, 5);
        const QRectF face = on ? full.adjusted(0, 2, 0, 0) : full.adjusted(0, 0, 0, -2);
        pnt.setBrush(on ? studio2AccentColor() : (over ? kKeyHover : kKeyFace));
        pnt.setPen(on ? QPen(kKeyOutline, 1.5) : QPen(kGoldEdge, 1));
        pnt.drawRoundedRect(face, 5, 5);
        pnt.setPen(on ? QColor(Qt::black) : kGold);
        QString text = local(ctl.label);
        if (showBind()) {
            const QString bind = shorten(bindingRaw(m_session, ctl.target));
            text = bind.isEmpty() ? text + QStringLiteral(" —") : text + QLatin1Char(' ') + bind.section(QStringLiteral(" / "), 0, 0);
        }
        drawFitted(pnt, face.adjusted(3, 0, -3, 0), text, pixelFont(font(), 10, true));
        pnt.setOpacity(1.0);
    }

    studio2session *m_session;
    std::function<void(int)> m_onPick;
    const studio2_control *m_layout = nullptr;
    int m_count = 0;
    int m_key[2][10];
    QRectF m_gold, m_ribbed, m_silver;
    std::vector<char> m_held;      /* per layout entry: a key or CLEAR the console sees held */
    bool m_showBindings = false;
    int m_map = -2;
    Hold m_hold = None;
    int m_holdIdx = -1;          /* the part the mouse holds */
    bool m_partDown = false;     /* the pointer is still over the part it pressed */
    int m_hover = -1;
    int m_panelW = 0, m_panelH = 0;
};

/* ---- ControllersWindow --------------------------------------------------------------- */

class ControllersWindow : public QWidget {
public:
    ControllersWindow(studio2session *session, QWidget *parent);
    void present();
    void refresh();

protected:
    void keyPressEvent(QKeyEvent *evt) override;
    void keyReleaseEvent(QKeyEvent *evt) override;
    void showEvent(QShowEvent *evt) override;
    void hideEvent(QHideEvent *evt) override;
    void changeEvent(QEvent *evt) override;
    /* Tab is a key of the console's, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void letGo();
    void setMapState(int state);
    void bound(int target, const QString &what, const QString &stolen);
    void pollCapture();
    void pollSlow();
    void rebuildPads();
    void refreshPadPlayers();

    studio2session *m_session;
    ConsoleWidget *m_console = nullptr;
    QWidget *m_padArea = nullptr;
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;       /* each pad's "plays keypad X" */
    unsigned m_padGen = ~0u;
    QPushButton *m_mapButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    ElidedLabel *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    uint32_t m_swallow = 0;               /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
     * gamepad button for that target. */
    int m_mapState = -2;
};

ControllersWindow::ControllersWindow(studio2session *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel
     * of fixed-size parts and nothing here wants more room. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers"));
    setProperty("studio2HelpContext", QStringLiteral("controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its parts and kept there: a resizable panel gets tiled and its
     * parts stretched. */
    root->setSizeConstraint(QLayout::SetFixedSize);

    m_console = new ConsoleWidget(session, [this](int target) { setMapState(target); });
    m_console->setObjectName(QStringLiteral("console"));
    root->addWidget(m_console, 0, Qt::AlignHCenter);

    /* Gamepads: one row per pad, rebuilt as they come and go, in room for a
     * fixed number so the window does not change size when one is plugged in */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    auto *padsBox = new QVBoxLayout(pads);
    m_padArea = new QWidget;
    m_padBox = new QVBoxLayout(m_padArea);
    m_padBox->setContentsMargins(0, 0, 0, 0);
    m_padBox->setSpacing(2);
    const int rowHeight = QComboBox().sizeHint().height() + 2;
    m_padArea->setFixedHeight(rowHeight * kMaxPadRows + m_padBox->spacing() * (kMaxPadRows - 1));
    m_padBox->setAlignment(Qt::AlignTop);
    padsBox->addWidget(m_padArea);
    root->addWidget(pads);

    auto *mapRow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    m_mapButton->setToolTip(QStringLiteral("Pick a part, then press a key or a gamepad button to give it"));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    m_clearButton = new QPushButton(QStringLiteral("Clear"));
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setToolTip(QStringLiteral("Take the picked control's key and gamepad button away"));
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (m_mapState < 0) return;
        studio2session_binding_clear(m_session, m_mapState);
        m_hint->setText(QStringLiteral("Cleared. Press a key or a gamepad button to bind it again"));
        m_console->update();
    });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    defaults->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    connect(defaults, &QPushButton::clicked, this, [this] {
        studio2session_bindings_reset(m_session);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every control is back to its default."));
    });
    mapRow->addWidget(m_mapButton);
    mapRow->addWidget(m_clearButton);
    mapRow->addWidget(defaults);
    mapRow->addStretch(1);
    mapRow->addWidget(studio2HelpButton(QStringLiteral("controllers")));
    root->addLayout(mapRow);

    auto *optRow = new QHBoxLayout;
    auto *bindings = new QCheckBox(QStringLiteral("Show bindings"));
    bindings->setFocusPolicy(Qt::NoFocus);
    bindings->setToolTip(QStringLiteral("Show the key each part has (hover for its gamepad button)"));
    bindings->setChecked(studio2session_get_int(session, "controllers_show_bindings", 0) > 0);
    connect(bindings, &QCheckBox::toggled, this, [this](bool checked) {
        studio2session_set_int(m_session, "controllers_show_bindings", checked ? 1 : 0);
        m_console->setShowBindings(checked);
    });
    m_console->setShowBindings(bindings->isChecked());
    optRow->addWidget(bindings);
    optRow->addStretch(1);
    root->addLayout(optRow);

    m_hint = new ElidedLabel(true, 2);
    root->addWidget(m_hint);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] { m_console->pollHeld(); });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
    letGo();
    m_console->pollHeld();
}

/* Lets go of what the mouse holds in this window. */
void ControllersWindow::letGo()
{
    m_console->releaseAll();
}

/* ---- Map mode -------------------------------------------------------------------------- */

void ControllersWindow::setMapState(int state)
{
    /* Entering (or changing) Map: nothing may stay held by the mouse. */
    letGo();
    m_mapState = state;
    m_clearButton->setEnabled(state >= 0);
    if (state == -2) {
        m_captureTimer.stop();
        studio2session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_mapButton->setStyleSheet(QString());
        m_hint->setText(QStringLiteral("Press a key with the mouse to hold it. CLEAR resets the machine while held."));
    } else if (state == -1) {
        m_captureTimer.stop();
        studio2session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_mapButton->setStyleSheet(QStringLiteral("background: %1; color: black;").arg(studio2AccentColor().name()));
        m_hint->setText(QStringLiteral("Click a part to remap it. Escape leaves Map."));
    } else {
        m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Escape cancels).")
                            .arg(local(studio2_target_name(state))));
        studio2session_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    m_console->setMap(m_mapState);
    m_console->pollHeld();
}

/* A binding was made: stay armed, since remapping several in a row is normal. */
void ControllersWindow::bound(int target, const QString &what, const QString &stolen)
{
    Q_UNUSED(target);
    setMapState(-1);
    m_hint->setText(stolen.isEmpty() ? QStringLiteral("Bound %1").arg(what)
                                     : QStringLiteral("Bound %1 (was %2)").arg(what, stolen));
}

void ControllersWindow::pollCapture()
{
    int button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (!studio2session_gamepad_capture_poll(m_session, &button)) return;
    char stolen[128] = "";
    const int target = m_mapState;
    studio2session_binding_set_button(m_session, target, button, stolen, sizeof stolen);
    bound(target, local(studio2_pad_button_name(button)), local(stolen));
}

/* ---- polling ---------------------------------------------------------------------------- */

/* Hot-plugged pads and auto-assignment changes. */
void ControllersWindow::pollSlow()
{
    const unsigned gen = studio2session_gamepad_generation(m_session);
    if (gen != m_padGen) {
        m_padGen = gen;
        rebuildPads();
    } else {
        refreshPadPlayers();
    }
}

/* ---- gamepads --------------------------------------------------------------------------- */

void ControllersWindow::refreshPadPlayers()
{
    for (int idx = 0; idx < (int)m_padEff.size(); ++idx) {
        const int eff = studio2session_gamepad_effective_port(m_session, idx);
        const QString text = eff >= 0 ? QStringLiteral("plays Keypad %1").arg(QChar(eff ? 'B' : 'A'))
                                      : QStringLiteral("plays no keypad");
        if (m_padEff[(size_t)idx]->text() != text) m_padEff[(size_t)idx]->setText(text);
    }
}

/* One row per pad: its name, the keypad it plays and the one it is set to. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *row : m_padRows) delete row;
    m_padRows.clear();
    m_padEff.clear();
    const int count = studio2session_gamepad_count(m_session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected - plug one in"));
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int idx = 0; idx < count && idx < kMaxPadRows; ++idx) {
        char name[128];
        studio2session_gamepad_name(m_session, idx, name, sizeof name);
        auto *row = new QWidget;
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto *label = new ElidedLabel(false);
        label->setText(local(name));
        label->setToolTip(local(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(QStringLiteral("color: gray;"));
        /* -1 = automatic (connection order), 0 = Keypad A, 1 = Keypad B */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Keypad A"), QStringLiteral("Keypad B") });
        const int assign = studio2session_gamepad_assignment(m_session, idx);
        box->setCurrentIndex(assign < 0 || assign > 1 ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, idx](int sel) {
            studio2session_gamepad_assign(m_session, idx, sel <= 0 ? -1 : sel - 1);
            refreshPadPlayers();
        });
        line->addWidget(label, 1);
        line->addWidget(eff);
        line->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
        m_padEff.push_back(eff);
    }
    if (count > kMaxPadRows) {
        /* the rest are in Preferences > Input */
        m_padEff.back()->setToolTip(QStringLiteral("%1 more gamepads are listed in Preferences").arg(count - kMaxPadRows));
    }
    refreshPadPlayers();
}

/* ---- the window ------------------------------------------------------------------------- */

void ControllersWindow::present()
{
    m_padGen = studio2session_gamepad_generation(m_session);
    rebuildPads();
    show();
    raise();
}

void ControllersWindow::refresh()
{
    if (isVisible()) {
        letGo();
        m_console->pollHeld();
    }
}

void ControllersWindow::showEvent(QShowEvent *evt)
{
    m_console->pollHeld();
    m_heldTimer.start(40);      /* 25 Hz */
    m_slowTimer.start(250);
    QWidget::showEvent(evt);
}

void ControllersWindow::hideEvent(QHideEvent *evt)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    letGo();
    m_swallow = 0;
    QWidget::hideEvent(evt);
}

/* Only what this window holds: the main window releases its own keys. */
void ControllersWindow::changeEvent(QEvent *evt)
{
    if (evt->type() == QEvent::ActivationChange && !isActiveWindow()) {
        letGo();
        studio2session_release_all(m_session);
        m_swallow = 0;
    }
    QWidget::changeEvent(evt);
}

void ControllersWindow::keyPressEvent(QKeyEvent *evt)
{
    if (evt->key() == Qt::Key_F9) {
        if (!evt->isAutoRepeat()) hide();
        return;
    }
    /* F9, F11 and F12 are never bindings: in Map they only explain. */
    if (evt->key() == Qt::Key_F11 || evt->key() == Qt::Key_F12) {
        if (evt->isAutoRepeat()) return;
        if (m_mapState >= 0) {
            m_hint->setText(QStringLiteral("F9, F11 and F12 are reserved and cannot be bound."));
            return;
        }
        if (evt->key() == Qt::Key_F12) DebuggerWindow::toggleFor(parentWidget(), m_session);
        return;
    }
    const uint32_t keysym = studio2KeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) return;

    if (m_mapState != -2) {
        if (evt->isAutoRepeat()) return;
        /* Esc backs out one step: a pending target, then Map itself */
        if (evt->key() == Qt::Key_Escape) {
            m_swallow = keysym;
            setMapState(m_mapState >= 0 ? -1 : -2);
            return;
        }
        if (m_mapState < 0 || !keysym) return;      /* armed, or nothing to bind: nothing is typed */
        char name[48] = "", stolen[128] = "";
        const int target = m_mapState;
        studio2session_binding_set_key(m_session, target, keysym, stolen, sizeof stolen);
        studio2session_keysym_name(keysym, name, sizeof name);
        m_swallow = keysym;
        bound(target, local(name), local(stolen));
        return;
    }
    /* Otherwise what the main window does with it: a console action goes to
     * the main window's queue, as a gamepad's does. */
    if (evt->isAutoRepeat()) return;
    if (!keysym) { QWidget::keyPressEvent(evt); return; }
    const int sysact = studio2session_key_sysaction(m_session, keysym);
    if (sysact >= 0) { studio2session_sysaction_post(m_session, sysact); return; }
    if (!studio2session_key(m_session, keysym, 1)) QWidget::keyPressEvent(evt);
}

/* A release always goes to the session, in Map mode too: it only lets go of
 * what a press asserted, so a key held while Map was armed cannot stick. */
void ControllersWindow::keyReleaseEvent(QKeyEvent *evt)
{
    if (evt->isAutoRepeat()) return;
    const uint32_t keysym = studio2KeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) {
        m_swallow = 0;
        return;
    }
    if (!keysym || !studio2session_key(m_session, keysym, 0)) QWidget::keyReleaseEvent(evt);
}

QPointer<ControllersWindow> g_window;

} // namespace

void studio2ControllersToggle(QWidget *parent, studio2session *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (g_window->isVisible()) g_window->hide();
    else g_window->present();
}

void studio2ControllersShow(QWidget *parent, studio2session *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (!g_window->isVisible()) g_window->present();
    else g_window->raise();
}

bool studio2ControllersVisible()
{
    return g_window && g_window->isVisible();
}

bool studio2ControllersActive()
{
    return g_window && g_window->isActiveWindow();
}

void studio2ControllersRefresh()
{
    if (g_window) g_window->refresh();
}
