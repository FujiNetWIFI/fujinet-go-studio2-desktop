/*
 * Debugger window (Qt6 Widgets) over the CDP1802 / CDP1861 debugger engine,
 * via core/include/studio2debug.h. Mirrors the GNOME one tab for tab, and
 * follows the SMS, Astrocade, Apple II, MSX and ADAM KDE debuggers.
 *
 * Tabs, in this order (STUDIO2_DEBUGGER_TAB picks the one a window opens on,
 * by index or by name -- 0 console, 1 disassembly, 2 memory, 3 breakpoints,
 * 4 watches, 5 cart, 6 trace, 7 history, 8 video, 9 sound, 10 symbols,
 * 11 fujinet -- or by the start of a label): the Console (the engine's
 * command line, with a history and Tab completion), the Disassembly (click a
 * line to select it for Run to Cursor; click its left edge, double-click it
 * or press F9 for its breakpoint), Memory (the whole 64K the CPU sees, Go to
 * a label / address / range, a hex or text search, Poke), Breakpoints
 * (execute, read, write, access, port IN and OUT, with a condition),
 * Watches (a value list, Break on change), Cart & I/O (the cartridge, its
 * 256-page map and bank window, the text engine, the keypads, the ports),
 * Trace and History (the engine's instruction ring: live, and read at a
 * stop), Video (the CDP1861's pictures and DMA), Sound (Q and the beeper's
 * pitch), Symbols (every label, filtered) and FujiNet (the FujiBus packets on
 * the cartridge's link). The registers sit in a strip under the tabs, on
 * every one of them. Any tab can be popped out into a window of its own
 * (STUDIO2_POPOUT names one at start). The toolbar carries the stepping
 * controls, Break at, symbol loading and Save; the family's keys apply (F5
 * run/stop, F7 step, F8 step over, Shift+F8 step out, Shift+F7 frame, F9
 * breakpoint, F12 close).
 *
 * Showing the window attaches (which stops the machine, as on every
 * sibling); hiding it detaches, which takes the breakpoints out of the core
 * and lets the machine run on.
 *
 * A short timer polls the engine's generation and stop serial and refreshes
 * the tab showing when they moved; while the machine runs, the live tabs
 * refresh every 400 ms. Every new stop re-centres every view on the spot.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"
#include "VideoTab.h"
#include "../FujiNetWindows.h"
#include "../help/HelpWindow.h"

extern "C" {
#include "studio2memtools.h"
}

#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QHideEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QThread>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>


#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_ROWS 4096           /* the whole 64K, 16 bytes a row */
#define MEM_ROW_TEXT 140        /* bytes of text a Memory row can take */
#define BP_MAX 256
#define TRACE_MAX 512
#define HIST_SHOWN 2000         /* trace lines shown */
#define RING_MAX 65537
#define CONSOLE_LINES 5000
#define REGION_MAX 64

namespace {

enum {
    TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_CART, TAB_TRACE, TAB_HISTORY, TAB_VIDEO,
    TAB_SOUND, TAB_SYMBOLS, TAB_FUJINET, TAB_COUNT
};

/* The tabs, in order: the names STUDIO2_DEBUGGER_TAB takes, their labels (which
 * it takes too), and their Help contexts (debugger/<help>). */
const struct { const char *name, *label, *help; } kTabs[TAB_COUNT] = {
    { "console", "Console", "console" },           { "disassembly", "Disassembly", "disassembly" },
    { "memory", "Memory", "memory" },              { "breakpoints", "Breakpoints", "breakpoints" },
    { "watches", "Watches", "watches" },           { "cart", "Cart & I/O", "cart" },
    { "trace", "Trace", "trace" },                 { "history", "History", "history" },
    { "video", "Video", "video" },                 { "sound", "Sound", "sound" },
    { "symbols", "Symbols", "symbols" },           { "fujinet", "FujiNet", "fujinet" },
};

/* The register entries below the sixteen scratchpad ones, in the order
 * shown, and the studio2debug_reg each sets. */
const struct { const char *name; int id, digits; const char *tip; } kRegs[] = {
    { "PC", STUDIO2_REG_PC, 4, "R(P): the instruction about to run" },
    { "D", STUDIO2_REG_D, 2, "The accumulator" },
    { "DF", STUDIO2_REG_DF, 1, "The carry (0 or 1)" },
    { "P", STUDIO2_REG_P, 1, "Which register is the program counter" },
    { "X", STUDIO2_REG_X, 1, "Which register is the data pointer" },
    { "T", STUDIO2_REG_T, 2, "X and P as an interrupt saved them" },
    { "B", STUDIO2_REG_B, 2, "The auxiliary register" },
    { "Q", STUDIO2_REG_Q, 1, "The Q output: the Studio II's beeper (0 or 1)" },
    { "IE", STUDIO2_REG_IE, 1, "Interrupts enabled (0 or 1)" },
};

/* The breakpoint types the Breakpoints tab offers, in the order of its chooser. */
const struct { const char *label; int type; } kBpTypes[6] = {
    { "Execute", STUDIO2DEBUG_BP_EXEC },     { "Read", STUDIO2DEBUG_BP_READ },       { "Write", STUDIO2DEBUG_BP_WRITE },
    { "Access", STUDIO2DEBUG_BP_ACCESS },    { "Port IN", STUDIO2DEBUG_BP_IN },      { "Port OUT", STUDIO2DEBUG_BP_OUT },
};

const QString kGray = QStringLiteral("color: gray;");

/* The PC's line and a marked memory row: the Studio II icon's tan. It is
 * pale, so every fill of it carries dark ink. */
QColor accentColor()
{
    return studio2AccentColor();
}

QColor accentInk()
{
    return QColor(Qt::black);
}

QTextCharFormat accentFormat()
{
    QTextCharFormat f;
    f.setBackground(accentColor());
    f.setForeground(accentInk());
    return f;
}

/* A watched range in the dump: a blue the accent does not hide. */
QTextCharFormat watchFormat()
{
    QTextCharFormat f;
    f.setBackground(QColor(0x1c, 0x71, 0xd8));
    f.setForeground(Qt::white);
    return f;
}

void setMono(QWidget *w)
{
    QFont f = w->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    w->setFont(f);
}

QPlainTextEdit *monoView()
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(true);
    setMono(v);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

/* Engine text for a view: control bytes dropped. */
QString stripControl(const char *s)
{
    const QString in = QString::fromUtf8(s);
    QString out;
    out.reserve(in.size());
    for (const QChar c : in)
        if (c.unicode() >= 0x20 || c == QLatin1Char('\n') || c == QLatin1Char('\t')) out += c;
    return out;
}

QString hx(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

QString yesNo(int v)
{
    return v ? QStringLiteral("yes") : QStringLiteral("no");
}

/* "Stopped" and the engine's reason: "stopped at $0038" reads "Stopped at
 * $0038", anything else "Stopped: <reason>". */
QString stoppedText(const QString &reason)
{
    if (reason.isEmpty()) return QStringLiteral("Stopped");
    if (reason.startsWith(QLatin1String("stopped"))) return QStringLiteral("S") + reason.mid(1);
    return QStringLiteral("Stopped: ") + reason;
}

QLabel *titleLabel(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

QLabel *hintLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setStyleSheet(kGray);
    l->setWordWrap(true);
    return l;
}

QLineEdit *smallEntry(const QString &placeholder, int chars)
{
    auto *e = new QLineEdit;
    e->setPlaceholderText(placeholder);
    e->setFixedWidth(e->fontMetrics().averageCharWidth() * chars + 16);
    return e;
}

/* A register entry exactly wide enough for its hex digits. */
void setDigits(QLineEdit *e, int digits)
{
    e->setFixedWidth(e->fontMetrics().horizontalAdvance(QString(digits, QLatin1Char('0'))) + 16);
}

/* A toggle takes the checked style (the accent, a box of its own), which is
 * not the native button's: one size for both states and every text,
 * so it never jumps. */
void fixToggleSize(QPushButton *b, const QStringList &texts, int minHeight)
{
    b->ensurePolished();
    QSize s(0, minHeight);
    for (const bool on : { false, true })
        for (const QString &t : texts) {
            b->setChecked(on);
            b->setText(t);
            s = s.expandedTo(b->sizeHint());
        }
    b->setChecked(false);
    b->setText(texts.first());
    b->setFixedSize(s);
}

/* Whether a view's reader is at its end (a log then follows new lines). */
bool atEnd(QPlainTextEdit *v)
{
    return v->verticalScrollBar()->value() >= v->verticalScrollBar()->maximum() - 1;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
void setText(QPlainTextEdit *v, const QString &text, bool follow = false)
{
    if (v->toPlainText() == text) {
        if (follow) v->verticalScrollBar()->setValue(v->verticalScrollBar()->maximum());
        return;
    }
    const int top = v->verticalScrollBar()->value(), left = v->horizontalScrollBar()->value();
    v->setPlainText(text);
    v->verticalScrollBar()->setValue(follow ? v->verticalScrollBar()->maximum() : top);
    v->horizontalScrollBar()->setValue(left);
}

/* A stretch of one line of a view in a format. */
QTextEdit::ExtraSelection spanSelection(QPlainTextEdit *v, int line, int from, int length, const QTextCharFormat &fmt)
{
    QTextEdit::ExtraSelection s;
    const QTextBlock block = v->document()->findBlockByNumber(line);
    s.cursor = QTextCursor(block);
    s.cursor.setPosition(block.position() + from);
    s.cursor.setPosition(block.position() + from + length, QTextCursor::KeepAnchor);
    s.format = fmt;
    return s;
}

/* STUDIO2_DEBUGGER_TAB: a page number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("i/o" ...). The names of the
 * tabs the older windows had still reach their successors. */
int tabFromEnv(const char *var = "STUDIO2_DEBUGGER_TAB")
{
    const QString tab = qEnvironmentVariable(var).trimmed();
    if (tab.isEmpty()) return -1;
    if (tab.compare(QLatin1String("io"), Qt::CaseInsensitive) == 0) return TAB_CART;
    if (tab.compare(QLatin1String("prompt"), Qt::CaseInsensitive) == 0) return TAB_CONSOLE;
    if (tab.compare(QLatin1String("cpu"), Qt::CaseInsensitive) == 0) return TAB_DISASM;
    bool isNum = false;
    const int n = tab.toInt(&isNum);
    if (isNum) return n >= 0 && n < TAB_COUNT ? n : -1;
    for (int pass = 0; pass < 3; ++pass)
        for (int i = 0; i < TAB_COUNT; ++i) {
            const QString name = QString::fromUtf8(kTabs[i].name), label = QString::fromUtf8(kTabs[i].label);
            if (pass == 0 && name.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 1 && label.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 2) {
                const QStringList words = label.split(QLatin1Char(' '));
                for (int w = 1; w < words.size(); ++w)
                    if (words[w].startsWith(tab, Qt::CaseInsensitive)) return i;
            }
        }
    return -1;
}

/* The family's keys: F5 run/stop, F7 step, Shift+F7 frame, F8 step over,
 * Shift+F8 step out, F9 breakpoint, F12 close. */
bool isDebuggerKey(const QKeyEvent *e)
{
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    switch (e->key()) {
    case Qt::Key_F5: case Qt::Key_F9: case Qt::Key_F12: return mods == Qt::NoModifier;
    case Qt::Key_F7: case Qt::Key_F8: return mods == Qt::NoModifier || mods == Qt::ShiftModifier;
    default: return false;
    }
}

QPointer<DebuggerWindow> s_win;

QFont monoFont(const QFont &base)
{
    QFont f = base;
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    return f;
}

/* What the cartridge serves in a page (S2PG_*: none, image, hotspot,
 * raster), as a colour and a name. */
const char *const kPageNames[4] = { "none", "image ROM", "hotspot", "raster (DMA)" };
QColor pageColour(int type, const QPalette &pal)
{
    switch (type) {
    case 1: return studio2AccentColor();
    case 2: return QColor(0xC0, 0x50, 0x4D);
    case 3: return QColor(0x4F, 0x81, 0xBD);
    default: return pal.color(QPalette::Base);
    }
}


/* The breakpoint type of a chooser row. */
int bpTypeAt(int row)
{
    return kBpTypes[qBound(0, row, 5)].type;
}

} // namespace

/* ---- the painted panels ------------------------------------------------------ */

/* One keypad as the console sees it, laid out as on the console (1 2 3 /
 * 4 5 6 / 7 8 9 / 0 under 8): the held keys filled in the accent colour,
 * the key OUT 2 has latched outlined. */
class KeypadDiagram : public QWidget {
public:
    explicit KeypadDiagram(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedSize(3 * kCell, 4 * kCell);
    }
    void set(unsigned held, int latch)
    {
        if (held == m_held && latch == m_latch) return;
        m_held = held;
        m_latch = latch;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        static const int keys[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, 0, -1 };
        QPainter p(this);
        for (int i = 0; i < 12; ++i) {
            const int k = keys[i];
            if (k < 0) continue;
            const QRect r((i % 3) * kCell + 1, (i / 3) * kCell + 1, kCell - 4, kCell - 4);
            const bool on = (m_held >> k) & 1u;
            p.fillRect(r, on ? studio2AccentColor() : palette().color(QPalette::Base));
            p.setPen(k == m_latch ? QPen(studio2AccentTextColor(this), 2) : QPen(palette().color(QPalette::Mid)));
            p.drawRect(r);
            /* the tan is pale: dark text on it, whatever the theme */
            p.setPen(on ? QColor(Qt::black) : palette().color(QPalette::Text));
            p.drawText(r, Qt::AlignCenter, QString::number(k));
        }
    }

private:
    static constexpr int kCell = 30;
    unsigned m_held = 0;
    int m_latch = -1;
};

/* The 256 pages of the address space, 4K to a row, coloured by what the
 * cartridge serves there; hover a cell for its range. */
class PageMapWidget : public QWidget {
public:
    explicit PageMapWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFont(monoFont(font()));
        const QFontMetrics fm(font());
        m_label = fm.horizontalAdvance(QStringLiteral("$F000 "));
        m_cw = qMax(16, fm.horizontalAdvance(QLatin1Char('0')) + 8);
        m_ch = qMax(14, fm.height());
        m_head = fm.height() + 2;
        int legend = 0;
        for (const char *n : kPageNames) legend += m_ch + 4 + fm.horizontalAdvance(QString::fromUtf8(n)) + 12;
        setFixedSize(qMax(m_label + 16 * m_cw + 1, legend), m_head + 16 * m_ch + 1 + 8 + m_ch);
    }
    void set(const uint8_t types[256])
    {
        if (!memcmp(types, m_types, sizeof m_types)) return;
        memcpy(m_types, types, sizeof m_types);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QPalette &pal = palette();
        p.setPen(pal.color(QPalette::Text));
        for (int col = 0; col < 16; ++col)
            p.drawText(QRect(m_label + col * m_cw, 0, m_cw, m_head), Qt::AlignCenter, hx(col, 1));
        for (int row = 0; row < 16; ++row) {
            p.setPen(pal.color(QPalette::Text));
            p.drawText(QRect(0, m_head + row * m_ch, m_label, m_ch), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("$%1").arg(hx(row * 0x1000, 4)));
            for (int col = 0; col < 16; ++col)
                p.fillRect(m_label + col * m_cw, m_head + row * m_ch, m_cw, m_ch,
                           pageColour(m_types[row * 16 + col], pal));
        }
        p.setPen(pal.color(QPalette::Mid));
        for (int i = 0; i <= 16; ++i) {
            p.drawLine(m_label + i * m_cw, m_head, m_label + i * m_cw, m_head + 16 * m_ch);
            p.drawLine(m_label, m_head + i * m_ch, m_label + 16 * m_cw, m_head + i * m_ch);
        }
        /* the legend */
        int x = 0;
        const int y = m_head + 16 * m_ch + 8;
        for (int t = 0; t < 4; ++t) {
            const QString name = QString::fromUtf8(kPageNames[t]);
            p.fillRect(x, y, m_ch, m_ch, pageColour(t, pal));
            p.setPen(pal.color(QPalette::Mid));
            p.drawRect(x, y, m_ch, m_ch);
            p.setPen(pal.color(QPalette::Text));
            const int w = QFontMetrics(font()).horizontalAdvance(name);
            p.drawText(QRect(x + m_ch + 4, y, w + 2, m_ch), Qt::AlignLeft | Qt::AlignVCenter, name);
            x += m_ch + 4 + w + 12;
        }
    }
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *he = static_cast<QHelpEvent *>(e);
            const int col = (he->pos().x() - m_label) / m_cw, row = (he->pos().y() - m_head) / m_ch;
            if (he->pos().x() >= m_label && he->pos().y() >= m_head && col < 16 && row < 16) {
                const int page = row * 16 + col;
                QToolTip::showText(he->globalPos(), QStringLiteral("$%1-$%2: %3")
                                       .arg(hx(page << 8, 4), hx((page << 8) | 0xFF, 4),
                                            QString::fromUtf8(kPageNames[m_types[page] & 3])), this);
            } else {
                QToolTip::hideText();
            }
            return true;
        }
        return QWidget::event(e);
    }

private:
    uint8_t m_types[256] = {};
    int m_label = 0, m_cw = 0, m_ch = 0, m_head = 0;
};

/* The cartridge's text engine as it has drawn: ten rows of sixteen cells,
 * white on black as on the television, the inverse cells inverted and the
 * cursor outlined in the accent colour. */
class TextGridWidget : public QWidget {
public:
    explicit TextGridWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFont(monoFont(font()));
        const QFontMetrics fm(font());
        m_cw = fm.horizontalAdvance(QLatin1Char('M')) + 6;
        m_ch = fm.height() + 2;
        setFixedSize(16 * m_cw + 8, 10 * m_ch + 8);
    }
    void set(const studio2debug_cart &c)
    {
        if (!memcmp(c.text, m_text, sizeof m_text) && !memcmp(c.text_inv, m_inv, sizeof m_inv)
            && c.cursor_row == m_row && c.cursor_col == m_col && c.present == m_present)
            return;
        memcpy(m_text, c.text, sizeof m_text);
        memcpy(m_inv, c.text_inv, sizeof m_inv);
        m_row = c.cursor_row;
        m_col = c.cursor_col;
        m_present = c.present;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (!m_present) return;
        for (int r = 0; r < 10; ++r) {
            for (int c = 0; c < 16; ++c) {
                const QRect cell(4 + c * m_cw, 4 + r * m_ch, m_cw, m_ch);
                const bool inv = m_inv[r][c] != 0;
                if (inv) p.fillRect(cell, Qt::white);
                p.setPen(inv ? Qt::black : Qt::white);
                p.drawText(cell, Qt::AlignCenter, glyph(m_text[r][c]));
            }
        }
        if (m_row >= 0 && m_row < 10 && m_col >= 0 && m_col < 16) {
            p.setPen(QPen(studio2AccentColor(), 2));
            p.drawRect(QRect(4 + m_col * m_cw, 4 + m_row * m_ch, m_cw, m_ch).adjusted(1, 1, -1, -1));
        }
    }

private:
    /* The engine's character set: $20-$7F its font, $01-$0B its symbols,
     * $80-$9F glyphs the app defined, $A0 a mosaic cell. */
    static QString glyph(uint8_t ch)
    {
        static const char16_t symbols[12] = { u' ', u'♠', u'♥', u'♦', u'♣', u'█', u'→',
                                              u'←', u'↑', u'↓', u'⑩', u'▒' };
        if (ch >= 0x20 && ch < 0x7F) return QString(QChar(ch));
        if (ch >= 0x01 && ch <= 0x0B) return QString(QChar(symbols[ch]));
        if (ch >= 0x80 && ch <= 0x9F) return QStringLiteral("□");
        if (ch == 0xA0) return QStringLiteral("▚");
        return QString();
    }

    uint8_t m_text[10][16] = {};
    uint8_t m_inv[10][16] = {};
    int m_row = -1, m_col = -1, m_present = 0;
    int m_cw = 0, m_ch = 0;
};

void DebuggerWindow::showFor(QWidget *parent, studio2session *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    /* showEvent attaches, which stops the machine */
    s_win->show();
    s_win->raise();
    s_win->activateWindow();
}

void DebuggerWindow::toggleFor(QWidget *parent, studio2session *session)
{
    if (s_win && s_win->isVisible()) s_win->hide();
    else showFor(parent, session);
}

DebuggerWindow::DebuggerWindow(studio2session *session, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(studio2session_debugger(session))
{
    setObjectName(QStringLiteral("debugger"));
    setWindowTitle(QStringLiteral("Studio II Debugger"));
    resize(1400, 820);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    m_tabs = new QTabWidget;
    m_tabs->setUsesScrollButtons(true);
    m_pages[TAB_CONSOLE] = buildConsole();
    m_pages[TAB_DISASM] = buildDisasm();
    m_pages[TAB_MEMORY] = buildMemory();
    m_pages[TAB_BREAKS] = buildBreaks();
    m_pages[TAB_WATCHES] = buildWatches();
    m_pages[TAB_CART] = buildCart();
    m_pages[TAB_TRACE] = buildHist(0);
    m_pages[TAB_HISTORY] = buildHist(1);
    m_pages[TAB_VIDEO] = buildVideo();
    m_pages[TAB_SOUND] = buildSound();
    m_pages[TAB_SYMBOLS] = buildSymbols();
    m_pages[TAB_FUJINET] = buildFujinet();
    for (int i = 0; i < TAB_COUNT; ++i) {
        /* a page's place in the tab widget is not its identity once tabs move
         * into windows of their own: each carries its own number */
        m_pages[i]->setProperty("tab", i);
        m_tabs->addTab(m_pages[i], QString::fromUtf8(kTabs[i].label).replace(QLatin1Char('&'), QLatin1String("&&")));
    }
    const int tab = tabFromEnv();
    if (tab >= 0) m_tabs->setCurrentIndex(tab);
    /* Help's contexts, one per tab */
    auto setHelpContext = [this](int page) {
        if (page >= 0 && page < TAB_COUNT)
            setProperty("studio2HelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[page].help)));
    };
    setHelpContext(currentTab());
    new QShortcut(QKeySequence(Qt::Key_F1), this, [this] { studio2HelpFor(this); });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this, setHelpContext](int) {
        const int page = currentTab();
        setHelpContext(page);
        if (page == TAB_VIDEO) m_videoForce = true;
        if (page == TAB_HISTORY) m_hSeen[1] = -1;
        refreshPage(page, false);
        if (page == TAB_CONSOLE) m_conIn->setFocus();
    });
    root->addWidget(m_tabs, 1);
    root->addWidget(buildRegs());
    setCentralWidget(central);
    statusBar();

    /* for captures and tests: STUDIO2_WATCH adds watches (a comma list),
     * STUDIO2_WATCH_BREAK a watch with a break-on-change over it, STUDIO2_MEM_GOTO
     * shows a range in Memory, STUDIO2_HISTORY records the trace and STUDIO2_POPOUT
     * peels a tab off */
    {
        const QByteArray mg = qgetenv("STUDIO2_MEM_GOTO");
        const QString wb = qEnvironmentVariable("STUDIO2_WATCH_BREAK").trimmed();
        for (const QString &text : qEnvironmentVariable("STUDIO2_WATCH").split(QLatin1Char(','), Qt::SkipEmptyParts))
            if (!text.trimmed().isEmpty()) studio2watch_add(text.trimmed().toUtf8().constData());
        if (!wb.isEmpty()) {
            const int index = studio2watch_add(wb.toUtf8().constData());
            if (index >= 0) watchBreak(index);
        }
        uint16_t lo, hi;
        if (!mg.isEmpty() && studio2sym_parse_range(m_dbg, mg.constData(), &lo, &hi)) showInMemory(lo, hi);
        if (tabFromEnv("STUDIO2_POPOUT") >= 0) popOut(tabFromEnv("STUDIO2_POPOUT"));
    }

    /* the timer runs only while the window is shown (showEvent/hideEvent) */
    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
}

/* Only at exit, with the main window: the session outlives its windows. */
DebuggerWindow::~DebuggerWindow()
{
    m_closing = true;       /* a peeled-off tab's window goes with the debugger */
    for (QWidget *&win : m_popped)
        if (win) {
            QWidget *w = win;
            win = nullptr;
            w->close();
        }
    if (studio2debug_is_attached(m_dbg)) studio2debug_detach(m_dbg);
}

/* ---- building ------------------------------------------------------------- */

/* Two rows: the stepping controls, Pop Out and Help; then Break at, the
 * symbols, Save and the status line, which has the room that is left and
 * wraps rather than being cut. */
QWidget *DebuggerWindow::buildToolbar()
{
    auto *bar = new QWidget;
    auto *rows = new QVBoxLayout(bar);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(2);
    auto *h = new QHBoxLayout;
    h->setContentsMargins(0, 0, 0, 0);
    rows->addLayout(h);
    auto add = [&](const QString &label, const QString &tip, std::function<void()> fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    /* Run is a checkable button, checked while the machine is stopped: its
     * stylesheet gives that the accent. */
    m_runBtn = add(QStringLiteral("Stop (F5)"), QStringLiteral("Run, or stop before the next instruction"),
                   [this] { toggleRun(); });
    m_runBtn->setCheckable(true);
    m_runBtn->setStyleSheet(
        QStringLiteral("QPushButton:checked { background:%1; color:black; }").arg(accentColor().name()));
    auto *step = add(QStringLiteral("Step (F7)"), QStringLiteral("One instruction"),
                     [this] { stepAnd([this] { studio2debug_step(m_dbg); }); });
    add(QStringLiteral("Step Over (F8)"), QStringLiteral("Over a subroutine call (a SEP to another register)"),
        [this] { stepAnd([this] { studio2debug_step_over(m_dbg); }); });
    add(QStringLiteral("Step Out (Shift+F8)"), QStringLiteral("On to the routine's return"),
        [this] { stepAnd([this] { studio2debug_step_out(m_dbg); }); });
    add(QStringLiteral("Run to Cursor"), QStringLiteral("On to the line selected in the Disassembly"),
        [this] { runToCursor(); });
    add(QStringLiteral("Scanline+1"), QStringLiteral("On to the next raster line"),
        [this] { stepAnd([this] { studio2debug_scanline(m_dbg, 1); }); });
    add(QStringLiteral("Frame+1 (Shift+F7)"), QStringLiteral("On to the end of the frame"),
        [this] { stepAnd([this] { studio2debug_frame(m_dbg, 1); }); });
    add(QStringLiteral("Clear Breakpoints"), QStringLiteral("Remove every breakpoint"), [this] {
        studio2debug_breakpoint_clear(m_dbg);
        resync();
    });
    fixToggleSize(m_runBtn, { QStringLiteral("Stop (F5)"), QStringLiteral("Run (F5)") }, step->sizeHint().height());
    h->addStretch(1);
    auto *pop = new QPushButton(QStringLiteral("Pop Out"));
    pop->setFocusPolicy(Qt::NoFocus);
    pop->setToolTip(QStringLiteral("Move the tab that shows into a window of its own; Dock puts it back"));
    connect(pop, &QPushButton::clicked, this, [this] { popOut(currentTab()); });
    h->addWidget(pop);
    auto *help = qobject_cast<QToolButton *>(studio2HelpButton(QString()));
    if (help) {
        help->setAutoRaise(false);
        help->setText(QStringLiteral("Help"));
        help->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    h->addWidget(help);

    auto *second = new QHBoxLayout;
    second->setContentsMargins(0, 0, 0, 0);
    rows->addLayout(second);
    second->addWidget(new QLabel(QStringLiteral("Break at:")));
    m_breakAt = smallEntry(QStringLiteral("symbol or $addr"), 18);
    m_breakAt->setToolTip(QStringLiteral("A symbol or a hex address, then Enter: an execute breakpoint there"));
    connect(m_breakAt, &QLineEdit::returnPressed, this, [this] { breakAt(); });
    second->addWidget(m_breakAt);

    auto *sym = new QPushButton(QStringLiteral("Load Symbols…"));
    sym->setFocusPolicy(Qt::NoFocus);
    sym->setToolTip(QStringLiteral("Labels from an AS .map or .lst, a WLA-DX .sym, or an \"AAAA name\" list"));
    auto *symMenu = new QMenu(sym);
    symMenu->addAction(QStringLiteral("Symbol File…"), this, [this] { loadSymbols(true); });
    symMenu->addAction(QStringLiteral("Next to the Cartridge"), this, [this] { loadSymbols(false); });
    sym->setMenu(symMenu);
    second->addWidget(sym);
    auto *clearSym = new QPushButton(QStringLiteral("Clear Symbols"));
    clearSym->setFocusPolicy(Qt::NoFocus);
    clearSym->setToolTip(QStringLiteral("Drop every loaded symbol and label; the built-in ones stay"));
    connect(clearSym, &QPushButton::clicked, this, [this] { clearSymbols(); });
    second->addWidget(clearSym);

    auto *saveBtn = new QPushButton(QStringLiteral("Save…"));
    saveBtn->setFocusPolicy(Qt::NoFocus);
    auto *menu = new QMenu(saveBtn);
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly (the BIOS and the cartridge's pages)", "disassembly.asm" },
        { "ram", "Console RAM ($0800-$09FF)", "ram.bin" },
        { "arena", "Mailbox Arena", "arena.bin" },
        { "raster", "Cartridge Raster (64x128)", "raster.bin" },
        { "screen", "Screen (PPM)", "screen.ppm" },
        { "regs", "CPU and Video State", "registers.txt" } };
    for (const auto &entry : saves) {
        const char *kind = entry.kind;
        const QString title = QString::fromUtf8(entry.title), file = QString::fromUtf8(entry.file);
        menu->addAction(title, this, [this, kind, title, file] { save(kind, title, file); });
    }
    saveBtn->setMenu(menu);
    second->addWidget(saveBtn);

    m_status = new QLabel;
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_status->setStyleSheet(kGray);
    m_status->setContentsMargins(8, 0, 8, 0);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    second->addWidget(m_status, 1);
    return bar;
}

QWidget *DebuggerWindow::buildConsole()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_conOut = monoView();
    m_conOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_conOut->setMaximumBlockCount(CONSOLE_LINES);
    m_conOut->setPlainText(QStringLiteral("Debugger console. Type 'help' for every command. Numbers: $0900 or 0x900 "
                                          "for hex, bare digits are decimal.\n"));
    m_conIn = new QLineEdit;
    setMono(m_conIn);
    m_conIn->setPlaceholderText(QStringLiteral("command (help, step, break, bpw, bpin, bpout, print, mem, poke, "
                                               "runto, disasm, video, cart ...) — Tab completes, Up/Down recall"));
    m_conIn->installEventFilter(this);
    connect(m_conIn, &QLineEdit::returnPressed, this, [this] { runConsole(); });
    v->addWidget(m_conOut, 1);
    v->addWidget(m_conIn);
    return w;
}

/* The registers, under the tabs and so on every one of them: R0-RF on the
 * first two rows, then R(P) and the rest of the CPU's state (Enter sets one),
 * then the next opcode's I and N, the external flags and the beam. */
QWidget *DebuggerWindow::buildRegs()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);

    auto *regs = new QGridLayout;
    regs->setHorizontalSpacing(8);
    regs->setVerticalSpacing(2);
    auto addReg = [&](int reg, QLabel *label, int digits, int row, int col, const QString &tip) {
        label->setStyleSheet(kGray);
        regs->addWidget(label, row, col, Qt::AlignRight);
        QLineEdit *e = m_reg[reg] = new QLineEdit;
        setMono(e);
        e->setMaxLength(12);
        setDigits(e, digits);
        e->setToolTip(tip);
        connect(e, &QLineEdit::returnPressed, this, [this, reg] { setReg(reg); });
        regs->addWidget(e, row, col + 1);
    };
    for (int n = 0; n < 16; ++n) {
        m_regLabel[n] = new QLabel(QStringLiteral("R%1").arg(hx((unsigned)n, 1)));
        addReg(STUDIO2_REG_R0 + n, m_regLabel[n], 4, n / 8, 2 * (n % 8),
               QStringLiteral("Enter sets it (hex, or an expression; only while stopped)"));
    }
    for (int i = 0; i < (int)(sizeof kRegs / sizeof kRegs[0]); ++i)
        addReg(kRegs[i].id, new QLabel(QString::fromUtf8(kRegs[i].name)), kRegs[i].digits, 2, 2 * i,
               QString::fromUtf8(kRegs[i].tip) + QStringLiteral(" (Enter sets it, only while stopped)"));
    regs->setColumnStretch(18, 1);
    v->addLayout(regs);

    /* the next opcode's I and N, and EF1-EF4: inputs, shown, not set */
    auto *flags = new QHBoxLayout;
    m_opcode = new QLabel;
    setMono(m_opcode);
    m_opcode->setToolTip(QStringLiteral("The instruction register of the opcode about to run: I (high nibble), "
                                        "N (low nibble)"));
    flags->addWidget(m_opcode);
    flags->addSpacing(16);
    for (int i = 0; i < 4; ++i) {
        m_ef[i] = new QCheckBox(QStringLiteral("EF%1").arg(i + 1));
        m_ef[i]->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_ef[i]->setFocusPolicy(Qt::NoFocus);
        m_ef[i]->setToolTip(QStringLiteral("An input: what the CPU reads on EF%1 now (EF1 is the 1861's display "
                                           "signal, EF3 and EF4 the keypads)").arg(i + 1));
        flags->addWidget(m_ef[i]);
    }
    flags->addSpacing(16);
    m_beam = new QLabel;
    setMono(m_beam);
    m_beam->setStyleSheet(kGray);
    flags->addWidget(m_beam, 1);
    v->addLayout(flags);
    return w;
}

/* The dump, Go to (a label, address or range, which it marks), the write
 * control, and the search bar. */
QWidget *DebuggerWindow::buildMemory()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    v->addWidget(titleLabel(QStringLiteral("Memory (the 64K as the CPU sees it)")));
    m_mem = monoView();
    v->addWidget(m_mem, 1);

    auto *nav = new QHBoxLayout;
    nav->addWidget(new QLabel(QStringLiteral("Go to")));
    m_memGoto = smallEntry(QStringLiteral("$0900, label or lo-hi"), 20);
    connect(m_memGoto, &QLineEdit::returnPressed, this, [this] { memGoto(); });
    nav->addWidget(m_memGoto);
    nav->addSpacing(12);
    nav->addWidget(new QLabel(QStringLiteral("Poke")));
    m_memAddr = smallEntry(QStringLiteral("$0900"), 10);
    m_memVal = smallEntry(QStringLiteral("$00"), 5);
    connect(m_memAddr, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    connect(m_memVal, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    nav->addWidget(m_memAddr);
    nav->addWidget(new QLabel(QStringLiteral("value")));
    nav->addWidget(m_memVal);
    nav->addWidget(hintLabel(QStringLiteral("Console RAM takes it; a page the cartridge serves takes it in the "
                                            "cartridge's own buffer")), 1);
    v->addLayout(nav);
    v->addWidget(buildSearchBar());
    return w;
}

QWidget *DebuggerWindow::buildSearchBar()
{
    auto *bar = new QWidget;
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(new QLabel(QStringLiteral("Search")));
    m_srchKind = new QComboBox;
    m_srchKind->addItems({ QStringLiteral("Hex"), QStringLiteral("Text") });
    m_srchKind->setToolTip(QStringLiteral("Hex bytes (3E 5A) or text, one byte per character"));
    row->addWidget(m_srchKind);
    m_srchEntry = new QLineEdit;
    setMono(m_srchEntry);
    m_srchEntry->setPlaceholderText(QStringLiteral("bytes or text to find"));
    connect(m_srchEntry, &QLineEdit::returnPressed, this, [this] { memSearch(false); });
    row->addWidget(m_srchEntry, 1);
    auto *next = new QPushButton(QStringLiteral("Find Next"));
    auto *prev = new QPushButton(QStringLiteral("Find Previous"));
    connect(next, &QPushButton::clicked, this, [this] { memSearch(false); });
    connect(prev, &QPushButton::clicked, this, [this] { memSearch(true); });
    row->addWidget(next);
    row->addWidget(prev);
    m_srchNote = new QLabel;
    m_srchNote->setStyleSheet(kGray);
    row->addWidget(m_srchNote);
    return bar;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_followPc = new QCheckBox(QStringLiteral("Follow PC"));
    m_followPc->setChecked(true);
    connect(m_followPc, &QCheckBox::clicked, this, [this] { refreshDisasm(); });
    m_jump = smallEntry(QStringLiteral("$address or label"), 20);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(); });
    row->addWidget(m_followPc);
    row->addSpacing(8);
    row->addWidget(new QLabel(QStringLiteral("Jump to")));
    row->addWidget(m_jump);
    row->addSpacing(8);
    row->addWidget(hintLabel(QStringLiteral("Click a line to select it (Run to Cursor); click its left edge, "
                                            "double-click it or press F9 for a breakpoint; scroll to browse")), 1);
    v->addLayout(row);
    m_disasm = monoView();
    /* no scrollbar: the wheel scrolls by instructions, as many lines as fit */
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->setFocusPolicy(Qt::NoFocus);
    m_disasm->viewport()->setCursor(Qt::ArrowCursor);
    m_disasm->viewport()->installEventFilter(this);
    v->addWidget(m_disasm, 1);
    return w;
}

QWidget *DebuggerWindow::buildVideo()
{
    m_video = new VideoTab(m_dbg);
    return m_video;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    m_bpStop = new QLabel;
    m_bpStop->setStyleSheet(QStringLiteral("font-weight: bold;"));
    m_bpStop->setWordWrap(true);
    v->addWidget(m_bpStop);

    v->addWidget(hintLabel(QStringLiteral("Execute stops before the instruction runs; the others after the "
                                          "access. A port breakpoint takes an I/O port (1-7: OUT 2 is the key "
                                          "latch, OUT 4 the cartridge's bank). Breakpoints are kept while the "
                                          "debugger is closed, but only act while it is open.")));

    m_bpTable = new QTableWidget(0, 4);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("Breakpoint"), QStringLiteral("Hits"),
                                           QString() });
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_bpTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_bpTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_bpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bpTable->setFocusPolicy(Qt::NoFocus);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling || item->column() != 0) return;
        studio2debug_breakpoint_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
        syncLater();
    });
    v->addWidget(m_bpTable, 1);
    m_bpEmpty = hintLabel(QStringLiteral("No breakpoints. Click the left edge of a disassembly line, or add one below."));
    v->addWidget(m_bpEmpty);

    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    for (const auto &entry : kBpTypes) m_bpType->addItem(QString::fromUtf8(entry.label));
    m_bpType->setToolTip(QStringLiteral("The port types take a port (1-7)"));
    m_bpStart = smallEntry(QStringLiteral("address, range or symbol"), 24);
    m_bpEnd = smallEntry(QStringLiteral("end (optional)"), 16);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional, e.g. d == 5 && value == $FF)"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto *clear = new QPushButton(QStringLiteral("Clear Breakpoints"));
    auto *clearHits = new QPushButton(QStringLiteral("Clear Hits"));
    connect(clearHits, &QPushButton::clicked, this, [this] {
        studio2debug_breakpoint_clear_hits(m_dbg);
        resync();
    });
    connect(add, &QPushButton::clicked, this, [this] { addBreakpoint(); });
    connect(clear, &QPushButton::clicked, this, [this] {
        studio2debug_breakpoint_clear(m_dbg);
        resync();
    });
    for (QLineEdit *e : { m_bpStart, m_bpEnd, m_bpCond })
        connect(e, &QLineEdit::returnPressed, this, [this] { addBreakpoint(); });
    row->addWidget(m_bpType);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    row->addWidget(clearHits);
    row->addWidget(clear);
    v->addLayout(row);

    m_bpMsg = new QLabel;
    m_bpMsg->setWordWrap(true);
    m_bpMsg->setStyleSheet(QStringLiteral("color: #e01b24;"));
    v->addWidget(m_bpMsg);
    return w;
}

QWidget *DebuggerWindow::buildWatches()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Watch")));
    m_wtEntry = new QLineEdit;
    setMono(m_wtEntry);
    m_wtEntry->setPlaceholderText(QStringLiteral("address, label, expression or lo-hi"));
    connect(m_wtEntry, &QLineEdit::returnPressed, this, [this] { addWatch(m_wtEntry->text()); });
    auto *add = new QPushButton(QStringLiteral("Add"));
    connect(add, &QPushButton::clicked, this, [this] { addWatch(m_wtEntry->text()); });
    row->addWidget(m_wtEntry, 1);
    row->addWidget(add);
    v->addLayout(row);

    m_wtTable = new QTableWidget(0, 2);
    m_wtTable->setHorizontalHeaderLabels({ QStringLiteral("Watch"), QString() });
    m_wtTable->verticalHeader()->setVisible(false);
    m_wtTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_wtTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_wtTable->setColumnWidth(1, 350);
    m_wtTable->verticalHeader()->setDefaultSectionSize(34);
    m_wtTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_wtTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_wtTable->setFocusPolicy(Qt::NoFocus);
    v->addWidget(m_wtTable, 1);
    v->addWidget(hintLabel(QStringLiteral("The values are read at a stop and show - while the machine runs. A watch "
                                          "is kept as text, so a label that loads later starts resolving. "
                                          "Break on change stops after a write that alters the value.")));
    return w;
}

/* The cartridge and the ports as text on the left; the page map, the text
 * engine's screen and the two keypads on the right. */
QWidget *DebuggerWindow::buildCart()
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    m_io = monoView();
    h->addWidget(m_io, 1);
    /* the painted panels: the page map in one column, the text engine and
     * the keypads in the next; the box scrolls, so it never sets the
     * window's minimum height */
    auto *rightBox = new QWidget;
    auto *columns = new QHBoxLayout(rightBox);
    auto *mapColumn = new QVBoxLayout;
    mapColumn->addWidget(titleLabel(QStringLiteral("Pages the cartridge serves")));
    m_pageMap = new PageMapWidget;
    mapColumn->addWidget(m_pageMap);
    mapColumn->addStretch();
    columns->addLayout(mapColumn);
    auto *right = new QVBoxLayout;
    columns->addLayout(right);
    right->addWidget(titleLabel(QStringLiteral("Text engine")));
    m_textGrid = new TextGridWidget;
    right->addWidget(m_textGrid);
    m_textNote = new QLabel;
    m_textNote->setStyleSheet(kGray);
    right->addWidget(m_textNote);
    right->addSpacing(12);
    right->addWidget(titleLabel(QStringLiteral("Keypads as the console sees them")));
    auto *pads = new QHBoxLayout;
    for (int k = 0; k < 2; ++k) {
        auto *col = new QVBoxLayout;
        auto *name = new QLabel(k ? QStringLiteral("Keypad B (EF4)") : QStringLiteral("Keypad A (EF3)"));
        name->setAlignment(Qt::AlignHCenter);
        col->addWidget(name);
        m_keypad[k] = new KeypadDiagram;
        col->addWidget(m_keypad[k], 0, Qt::AlignHCenter);
        pads->addLayout(col);
    }
    right->addLayout(pads);
    right->addWidget(hintLabel(QStringLiteral("Filled: held. Outlined: the key OUT 2 has latched.")));
    right->addStretch();
    auto *scroll = new QScrollArea;
    scroll->setWidget(rightBox);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedWidth(rightBox->sizeHint().width() + scroll->verticalScrollBar()->sizeHint().width() + 4);
    h->addWidget(scroll);
    return w;
}

/* Trace (idx 0) reads the instruction ring live; History (idx 1) reads it at
 * a stop. They share the one ring and recorder, so the two Record buttons
 * follow each other. */
QWidget *DebuggerWindow::buildHist(int idx)
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *bar = new QHBoxLayout;

    m_hRec[idx] = new QPushButton(QStringLiteral("Record"));
    m_hRec[idx]->setCheckable(true);
    m_hRec[idx]->setFocusPolicy(Qt::NoFocus);
    m_hRec[idx]->setToolTip(QStringLiteral("Record every instruction the CPU runs (the machine runs a little slower)"));
    m_hRec[idx]->setStyleSheet(
        QStringLiteral("QPushButton:checked { background:%1; color:black; }").arg(accentColor().name()));
    fixToggleSize(m_hRec[idx], { QStringLiteral("Record") }, m_hRec[idx]->sizeHint().height());
    connect(m_hRec[idx], &QPushButton::toggled, this, [this](bool on) {
        if (m_hSync) return;
        studio2debug_trace_enable(m_dbg, on);
        for (int i = 0; i < 2; ++i) refreshHist(i, true);
    });
    m_hFilter[idx] = new QComboBox;
    m_hFilter[idx]->addItems({ QStringLiteral("All instructions"), QStringLiteral("Branches only"),
                               QStringLiteral("In range") });
    m_hRange[idx] = new QLineEdit;
    setMono(m_hRange[idx]);
    m_hRange[idx]->setPlaceholderText(QStringLiteral("range: lo-hi or a label"));
    connect(m_hFilter[idx], QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, idx](int) { refreshHist(idx, true); });
    connect(m_hRange[idx], &QLineEdit::returnPressed, this, [this, idx] { refreshHist(idx, true); });
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        studio2debug_trace_clear(m_dbg);
        for (int i = 0; i < 2; ++i) refreshHist(i, true);
    });
    m_hNote[idx] = new QLabel;
    m_hNote[idx]->setStyleSheet(kGray);
    m_hNote[idx]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bar->addWidget(m_hRec[idx]);
    bar->addWidget(m_hFilter[idx]);
    bar->addWidget(m_hRange[idx], 1);
    bar->addWidget(clear);
    bar->addWidget(m_hNote[idx], 1);
    v->addLayout(bar);
    m_hText[idx] = monoView();
    v->addWidget(m_hText[idx], 1);
    return w;
}

QWidget *DebuggerWindow::buildSound()
{
    m_sound = monoView();
    return m_sound;
}

/* A filter over a table of address, name and bank; activating a row opens
 * that address in the Disassembly. */
QWidget *DebuggerWindow::buildSymbols()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Filter")));
    m_symFilter = new QLineEdit;
    m_symFilter->setPlaceholderText(QStringLiteral("part of a name"));
    m_symFilter->setClearButtonEnabled(true);
    connect(m_symFilter, &QLineEdit::textChanged, this, [this] { refreshSymbols(true); });
    row->addWidget(m_symFilter, 1);
    m_symCount = new QLabel;
    m_symCount->setStyleSheet(kGray);
    row->addWidget(m_symCount);
    v->addLayout(row);

    m_symTable = new QTableWidget(0, 3);
    m_symTable->setHorizontalHeaderLabels({ QStringLiteral("Address"), QStringLiteral("Name"), QStringLiteral("Bank") });
    m_symTable->verticalHeader()->setVisible(false);
    m_symTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_symTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_symTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_symTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_symTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    setMono(m_symTable);
    auto activate = [this](int row) {
        QTableWidgetItem *item = row >= 0 ? m_symTable->item(row, 0) : nullptr;
        if (item) showInDisassembly((uint16_t)item->data(Qt::UserRole).toUInt());
    };
    connect(m_symTable, &QTableWidget::cellDoubleClicked, this, [activate](int row, int) { activate(row); });
    connect(m_symTable, &QTableWidget::itemActivated, this, [activate](QTableWidgetItem *item) { activate(item->row()); });
    v->addWidget(m_symTable, 1);
    v->addWidget(hintLabel(QStringLiteral("Double-click a row (or press Enter on it) to open its address in the "
                                          "Disassembly.")));
    return w;
}

/* The packets left (a note and Clear over them), the link and counts
 * right. */
QWidget *DebuggerWindow::buildFujinet()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->addWidget(titleLabel(QStringLiteral("FujiBus packets (SLIP-decoded)")));
    auto *row = new QHBoxLayout;
    m_fujiNote = new QLabel;
    m_fujiNote->setStyleSheet(kGray);
    m_fujiNote->setWordWrap(true);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        studio2debug_fujinet_trace_clear(m_dbg);
        refreshFujinet(true);
    });
    row->addWidget(m_fujiNote, 1);
    row->addWidget(clear);
    lv->addLayout(row);
    m_fujiText = monoView();
    lv->addWidget(m_fujiText, 1);
    left->setMinimumWidth(560);

    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->addWidget(titleLabel(QStringLiteral("The cartridge's mailbox")));
    m_fujiState = monoView();
    rv->addWidget(m_fujiState, 1);
    right->setMinimumWidth(320);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);
    return split;
}

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus()
{
    char reason[160];
    int addr = -1;
    const bool stopped = studio2debug_is_stopped(m_dbg) != 0;
    reason[0] = '\0';
    studio2debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
    QString text = QStringLiteral("Running");
    if (stopped) {
        text = stoppedText(stripControl(reason));
        /* where, by name when it has one */
        char name[64];
        if (addr >= 0 && studio2debug_address_label(m_dbg, (uint16_t)addr, name, sizeof name) > 0 && name[0])
            text += QStringLiteral("  (%1)").arg(QString::fromUtf8(name));
    }
    if (m_status->text() != text) {
        m_status->setText(text);
        m_status->setToolTip(text);
    }
    const QString run = stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)");
    if (m_runBtn->text() != run) m_runBtn->setText(run);
    if (m_runBtn->isChecked() != stopped) m_runBtn->setChecked(stopped);
}

/* The refresh never overwrites a value being typed. */
void DebuggerWindow::setEntry(QLineEdit *e, const QString &text)
{
    if (e->hasFocus()) return;
    if (e->text() != text) e->setText(text);
}

void DebuggerWindow::refreshCpu()
{
    studio2debug_cpu c;
    studio2debug_cpu_get(m_dbg, &c);
    for (int n = 0; n < 16; ++n) {
        setEntry(m_reg[STUDIO2_REG_R0 + n], hx((unsigned)c.r[n] & 0xFFFF, 4));
        QFont f = m_regLabel[n]->font();
        f.setBold((c.p & 0xF) == n);
        f.setUnderline((c.x & 0xF) == n);
        m_regLabel[n]->setFont(f);
        QString tip;
        if ((c.p & 0xF) == n) tip = QStringLiteral("The program counter (P)");
        if ((c.x & 0xF) == n) tip += (tip.isEmpty() ? QString() : QStringLiteral(" and ")) + QStringLiteral("the data pointer (X)");
        m_regLabel[n]->setToolTip(tip);
    }
    setEntry(m_reg[STUDIO2_REG_PC], hx((unsigned)c.pc & 0xFFFF, 4));
    setEntry(m_reg[STUDIO2_REG_D], hx((unsigned)c.d & 0xFF, 2));
    setEntry(m_reg[STUDIO2_REG_DF], QString::number(c.df ? 1 : 0));
    setEntry(m_reg[STUDIO2_REG_P], hx((unsigned)c.p & 0xF, 1));
    setEntry(m_reg[STUDIO2_REG_X], hx((unsigned)c.x & 0xF, 1));
    setEntry(m_reg[STUDIO2_REG_T], hx((unsigned)c.t & 0xFF, 2));
    setEntry(m_reg[STUDIO2_REG_B], hx((unsigned)c.b & 0xFF, 2));
    setEntry(m_reg[STUDIO2_REG_Q], QString::number(c.q ? 1 : 0));
    setEntry(m_reg[STUDIO2_REG_IE], QString::number(c.ie ? 1 : 0));
    for (int i = 0; i < 4; ++i) m_ef[i]->setChecked(c.ef[i] != 0);
    uint8_t op = 0;
    if (studio2debug_read(m_dbg, (uint16_t)c.pc, &op, 1) == 1)
        m_opcode->setText(QStringLiteral("I:N = %1:%2").arg(hx(op >> 4, 1), hx(op & 0xF, 1)));
    else
        m_opcode->setText(QStringLiteral("I:N = -:-"));
    m_beam->setText(QStringLiteral("%1 / %2  line %3 dot %4  frame %5  %6 clocks%7%8")
                        .arg(QString::fromUtf8(c.state_name ? c.state_name : "?"),
                             QString::fromUtf8(c.mode_name ? c.mode_name : "?"))
                        .arg(c.vpos, 3).arg(c.hpos, 3).arg(c.frame).arg((qulonglong)c.cycles)
                        .arg(c.int_line ? QStringLiteral("  INT") : QString(),
                             c.dma_line ? QStringLiteral("  DMA OUT") : QString()));
}

/* The whole 64K as the CPU sees it, a row of 16 bytes at a time, each row
 * named by the memory-map region it starts in (one fetch of the map for the
 * refresh). The Go to / search range stays marked, and so does every watch. */
void DebuggerWindow::refreshMem()
{
    static uint8_t mem[MEM_ROWS * 16];
    static char text[MEM_ROWS * MEM_ROW_TEXT + 256];
    static studio2debug_region map[REGION_MAX];
    memset(mem, 0, sizeof mem);
    studio2debug_read(m_dbg, 0, mem, (int)sizeof mem);
    const int regions = studio2debug_memory_map(m_dbg, map, REGION_MAX);

    int len = snprintf(text, sizeof text, "       ");
    for (int col = 0; col < 16; ++col) len += snprintf(text + len, sizeof text - (size_t)len, "%X  ", col);
    for (int row = 0; row < MEM_ROWS; ++row) {
        const uint8_t *bytes = mem + row * 16;
        len += snprintf(text + len, sizeof text - (size_t)len, "\n$%04X: ", row * 16);
        for (int col = 0; col < 16; ++col) len += snprintf(text + len, sizeof text - (size_t)len, "%02X ", bytes[col]);
        text[len++] = ' ';
        for (int col = 0; col < 16; ++col) text[len++] = bytes[col] >= 0x20 && bytes[col] < 0x7F ? (char)bytes[col] : '.';
        char name[64];
        studio2mem_region_name(map, regions, (uint16_t)(row * 16), name, sizeof name);
        len += snprintf(text + len, sizeof text - (size_t)len, "  %s", name);
    }
    setText(m_mem, QString::fromLatin1(text, len));

    /* the ranges marked: each row's bytes in the hex and in the text column */
    QList<QTextEdit::ExtraSelection> sel;
    auto mark = [&](uint16_t from, uint16_t to, const QTextCharFormat &fmt) {
        for (int row = from / 16; row <= to / 16; ++row) {
            const int first = qMax((int)from - row * 16, 0), last = qMin((int)to - row * 16, 15);
            sel << spanSelection(m_mem, 1 + row, 7 + first * 3, (last - first) * 3 + 2, fmt);
            sel << spanSelection(m_mem, 1 + row, 7 + 48 + 1 + first, last - first + 1, fmt);
        }
    };
    for (int i = 0; i < studio2watch_count(); ++i) {
        uint16_t lo, hi;
        if (studio2watch_range(m_dbg, i, &lo, &hi)) mark(lo, hi, watchFormat());
    }
    if (m_hl) mark(m_hlLo, m_hlHi, accentFormat());
    m_mem->setExtraSelections(sel);
}

/* Memory scrolled so the row holding addr sits a third of the way down. */
void DebuggerWindow::scrollMemory(uint16_t addr)
{
    const int line = 1 + addr / 16;
    const int shown = m_mem->viewport()->height() / qMax(1, m_mem->fontMetrics().lineSpacing());
    m_mem->verticalScrollBar()->setValue(qMax(0, line - shown * 3 / 10));
}

/* The disassembly lines that fit the view. */
int DebuggerWindow::visibleRows() const
{
    const int line = m_disasm->fontMetrics().lineSpacing();
    const int height = m_disasm->viewport()->height() - 2 * (int)m_disasm->document()->documentMargin();
    if (line <= 0 || height <= 0) return 32;
    return qBound(4, height / line, DISASM_WINDOW);
}

void DebuggerWindow::refreshDisasm()
{
    static studio2debug_line lines[DISASM_WINDOW];
    const int rows = visibleRows();
    if (m_followPc->isChecked()) {
        studio2debug_cpu c;
        studio2debug_cpu_get(m_dbg, &c);
        /* the PC a third of the way down */
        m_disasmTop = (uint16_t)studio2debug_row_address(m_dbg, (uint16_t)c.pc, -(rows / 3));
    }
    int pcLine = -1;
    const int n = studio2debug_disassemble(m_dbg, m_disasmTop, lines, rows, &pcLine);
    /* the label column is as wide as the longest label shown, within reason */
    int labelWidth = 0;
    for (int i = 0; i < n; ++i) labelWidth = qMax(labelWidth, (int)strlen(lines[i].label));
    labelWidth = qMin(labelWidth, 24);

    m_lineAddr.clear();
    QString text;
    char buf[128];
    struct Span { int line, from, length; };
    std::vector<Span> grays;
    for (int i = 0; i < n; ++i) {
        m_lineAddr.push_back(lines[i].address);
        /* the gutter: a breakpoint dot (hollow when disabled), then the
         * PC's arrow */
        QString ln = lines[i].has_breakpoint ? QStringLiteral("●") : lines[i].bp_disabled ? QStringLiteral("○")
                                                                                          : QStringLiteral(" ");
        snprintf(buf, sizeof buf, "%c%04X  %-11.11s ", lines[i].is_pc ? '>' : ' ', lines[i].address, lines[i].bytes);
        ln += QString::fromUtf8(buf);
        const int labelFrom = (int)ln.size();
        char label[48];
        snprintf(label, sizeof label, "%-*.*s ", labelWidth, labelWidth, lines[i].label);
        ln += QString::fromUtf8(label);
        if (lines[i].label[0] && i != pcLine)
            grays.push_back({ i, labelFrom, (int)strlen(lines[i].label) < labelWidth ? (int)strlen(lines[i].label)
                                                                                      : labelWidth });
        snprintf(buf, sizeof buf, "%-22s", lines[i].disasm);
        ln += QString::fromUtf8(buf);
        if (lines[i].comment[0]) {
            const QString comment = QStringLiteral("; ") + QString::fromUtf8(lines[i].comment);
            if (i != pcLine) grays.push_back({ i, (int)ln.size(), (int)comment.size() });
            ln += comment;
        }
        if (i) text += QLatin1Char('\n');
        text += ln;
    }
    if (n == 0) text = QStringLiteral("(no disassembly: the debugger is not attached)");
    m_disasm->setPlainText(stripControl(text.toUtf8().constData()));
    m_disasm->verticalScrollBar()->setValue(0);

    /* The PC's line is the accent bar: the block's own background, not an
     * extra selection -- a full-width selection paints over the gutter's
     * red dot, a block background lies under it. */
    if (pcLine >= 0 && pcLine < n) {
        QTextCursor cur(m_disasm->document()->findBlockByNumber(pcLine));
        QTextBlockFormat bf;
        bf.setBackground(accentColor());
        cur.setBlockFormat(bf);
        cur.select(QTextCursor::LineUnderCursor);
        QTextCharFormat cf;
        cf.setForeground(accentInk());
        if (lines[pcLine].address == m_selected) {
            cf.setFontUnderline(true);
            cf.setFontWeight(QFont::Bold);
        }
        cur.mergeCharFormat(cf);
    }

    QList<QTextEdit::ExtraSelection> sel;
    /* labels and comments in the system's grey */
    QTextCharFormat grey;
    grey.setForeground(m_disasm->palette().color(QPalette::Disabled, QPalette::Text));
    for (const Span &g : grays) sel << spanSelection(m_disasm, g.line, g.from, g.length, grey);
    for (int i = 0; i < n; ++i) {
        /* the gutter: a breakpoint red (white on the PC's bar), a disabled one grey */
        if (lines[i].has_breakpoint || lines[i].bp_disabled) {
            QTextCharFormat f;
            if (lines[i].has_breakpoint) {
                f.setForeground(i == pcLine ? QColor(0xa0, 0x10, 0x18) : QColor(0xe0, 0x1b, 0x24));
                f.setFontWeight(QFont::Bold);
            } else {
                f.setForeground(i == pcLine ? QColor(0x55, 0x55, 0x55) : QColor(0x9a, 0x99, 0x96));
            }
            sel << spanSelection(m_disasm, i, 0, 1, f);
        }
        /* the selected line underlined (the PC's own is done above) */
        if (lines[i].address == m_selected && i != pcLine) {
            QTextCharFormat f;
            f.setFontUnderline(true);
            f.setFontWeight(QFont::Bold);
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(i));
            s.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            s.format = f;
            sel << s;
        }
    }
    m_disasm->setExtraSelections(sel);
}

/* The Video tab takes its own snapshot; a stopped machine redraws only when
 * the core says something changed. */
void DebuggerWindow::refreshVideo(bool force)
{
    m_video->refresh(force);
}

void DebuggerWindow::refreshBps()
{
    static studio2debug_breakpoint bps[BP_MAX];
    const int n = studio2debug_breakpoint_list(m_dbg, bps, BP_MAX);

    if (studio2debug_is_stopped(m_dbg)) {
        char reason[160];
        int addr = -1;
        reason[0] = '\0';
        studio2debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
        m_bpStop->setText(stoppedText(stripControl(reason)) + QLatin1Char('.'));
    } else {
        m_bpStop->setText(QStringLiteral("Running."));
    }

    /* rebuilt only when it changed (each step refreshes), and never while a
     * row's own button is being handled */
    QString sig;
    for (int i = 0; i < n; ++i)
        sig += QStringLiteral("%1 %2 %3 %4 %5 %6 %7\n").arg(bps[i].id).arg(bps[i].enabled).arg(bps[i].hits)
                   .arg(bps[i].type).arg(bps[i].start).arg(bps[i].end).arg(QString::fromUtf8(bps[i].condition));
    if (sig == m_bpSig && m_bpTable->rowCount() == n) return;
    m_bpSig = sig;

    QFont mono = m_bpTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    m_bpFilling = true;
    m_bpTable->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        on->setCheckState(bps[i].enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, bps[i].id);
        on->setToolTip(QStringLiteral("Enabled"));
        m_bpTable->setItem(i, 0, on);
        char desc[256];
        studio2bp_describe(m_dbg, &bps[i], desc, sizeof desc);
        m_bpTable->setItem(i, 1, new QTableWidgetItem(stripControl(desc)));
        m_bpTable->setItem(i, 2, new QTableWidgetItem(QString::number(bps[i].hits)));
        for (int col = 1; col < 3; ++col) m_bpTable->item(i, col)->setFont(mono);
        auto *rm = new QToolButton;
        rm->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete"), QIcon::fromTheme(QStringLiteral("user-trash"))));
        if (rm->icon().isNull()) rm->setText(QStringLiteral("Remove"));
        rm->setAutoRaise(true);
        rm->setToolTip(QStringLiteral("Remove"));
        const int id = bps[i].id;
        connect(rm, &QToolButton::clicked, this, [this, id] {
            studio2debug_breakpoint_remove(m_dbg, id);
            syncLater();
        });
        m_bpTable->setCellWidget(i, 3, rm);
    }
    m_bpFilling = false;
    m_bpEmpty->setVisible(n == 0);
}

/* The cartridge, the memory map as regions (drawn here once), then the
 * console's ports and keypads. */
void DebuggerWindow::refreshCart()
{
    static studio2debug_region reg[REGION_MAX];
    studio2debug_cart c;
    studio2debug_io io;
    char info[256];
    studio2debug_cart_get(m_dbg, &c);
    studio2debug_cart_info(m_dbg, info, sizeof info);
    studio2debug_io_get(m_dbg, &io);
    m_pageMap->set(c.page_type);
    m_textGrid->set(c);

    QString s = stripControl(info) + QStringLiteral("\n\n");
    if (c.present) {
        m_textNote->setText(QStringLiteral("cursor row %1 column %2%3").arg(c.cursor_row).arg(c.cursor_col)
                                .arg(c.inverse ? QStringLiteral(", drawing inverse") : QString()));
        s += QStringLiteral("Mode          %1 (%2)%3\n").arg(QString::fromUtf8(c.mode_name ? c.mode_name : "?")).arg(c.mode)
                 .arg(c.direct ? QStringLiteral(", an opened cartridge file") : QString());
        s += QStringLiteral("Booted game   %1\n").arg(yesNo(c.booted_game));
        s += QStringLiteral("Mailbox mode  %1\n").arg(c.mailbox_mode ? QStringLiteral("yes (a socket and fujimail)")
                                                                     : QStringLiteral("no (a game alone)"));
        s += QStringLiteral("Bank window   %1\n").arg(c.nbanks > 0 ? QStringLiteral("bank %1 of %2 at $C400").arg(c.bank).arg(c.nbanks)
                                                                  : QStringLiteral("none"));
        s += QStringLiteral("Link          %1%2\n\n").arg(c.link_up ? "up" : "down",
                                                         c.busy ? QStringLiteral(" (transaction in flight)") : QString());
        s += QStringLiteral("Live image    %1, %2 bytes, %3 blocks\n              CRC-32 %4%5\n")
                 .arg(c.live_st2 ? QStringLiteral("ST2") : QStringLiteral("raw")).arg(c.live_size).arg(c.live_blocks)
                 .arg(hx(c.live_crc, 8), c.live_mailbox ? QStringLiteral(", claims the mailbox") : QString());
        s += QStringLiteral("Staged        %1%2").arg(c.staged ? (c.staged_config ? QStringLiteral("CONFIG")
                                                                     : c.staged_claim ? QStringLiteral("an app")
                                                                                      : QStringLiteral("a game"))
                                                               : QStringLiteral("nothing"),
                                                      c.armed ? QStringLiteral(" (armed)") : QString());
        if (c.staged) s += QStringLiteral(", CRC-32 %1").arg(hx(c.staged_crc, 8));
        s += QStringLiteral("   swaps %1\n\n").arg(c.swaps);
        s += QStringLiteral("Mailbox       ACKSEQ $%1   STATUS $%2   last error %3\n              reply cmd $%4   RXLEN %5\n")
                 .arg(hx(c.ackseq, 2), hx(c.status, 2)).arg(c.last_error).arg(hx(c.reply_cmd, 2)).arg(c.rxlen);
        s += QStringLiteral("Boot          state %1   %2%   error %3   %4 of %5 bytes\n")
                 .arg(c.boot_state).arg(c.boot_pct).arg(c.boot_err).arg(c.boot_got).arg(c.boot_total);
        s += QStringLiteral("Raster        %1 frames served (mod 256)   short bursts %2\n").arg(c.frames).arg(c.short_lines);
        s += QStringLiteral("Queue depth   %1\n").arg(c.queue_depth);
    } else {
        m_textNote->clear();
    }

    s += QStringLiteral("\nMemory map\n");
    const int n = studio2debug_memory_map(m_dbg, reg, REGION_MAX);
    for (int i = 0; i < n; ++i)
        s += QStringLiteral("  $%1-$%2  %3%4  %5\n").arg(hx(reg[i].lo, 4), hx(reg[i].hi, 4))
                 .arg(reg[i].readable ? QLatin1Char('r') : QLatin1Char('-'))
                 .arg(reg[i].writable ? QLatin1Char('w') : QLatin1Char('-'))
                 .arg(QString::fromUtf8(reg[i].name));

    s += QStringLiteral("\nPorts and keys\n");
    s += QStringLiteral("  Key latch              %1 (OUT 2)\n").arg(io.keylatch);
    s += QStringLiteral("  EF3 / EF4              %1 / %2   (the latched key on keypad A / B)\n").arg(io.ef3).arg(io.ef4);
    s += QStringLiteral("  CLEAR                  %1\n").arg(io.clear_held ? "held" : "up");
    for (int k = 0; k < 2; ++k) {
        QString keys;
        for (int i = 0; i < 10; ++i)
            if (io.keys[k] & (1u << i)) keys += QStringLiteral(" %1").arg(i);
        s += QStringLiteral("  Keypad %1               %2\n").arg(QChar(k ? 'B' : 'A'))
                 .arg(keys.isEmpty() ? QStringLiteral("(none held)") : keys.mid(1));
    }
    s += QStringLiteral("  Last OUT              ");
    for (int port = 1; port < 8; ++port) s += QStringLiteral(" %1=$%2").arg(port).arg(hx(io.last_out[port], 2));
    s += QStringLiteral("\n  INP 1                  %1 (display on)\n").arg(io.inp1_count);
    s += QStringLiteral("  BIOS                   %1\n")
             .arg(io.have_bios ? QStringLiteral("the real BIOS runs")
                               : io.stub_parked ? QStringLiteral("the stand-in, parked: a game needs the BIOS")
                                                : QStringLiteral("the stand-in (CONFIG and FujiNet apps)"));
    setText(m_io, s);
    const int latch = io.keylatch < 10 ? io.keylatch : -1;
    for (int k = 0; k < 2; ++k) m_keypad[k]->set(io.keys[k], latch);
}

/* Q and the beeper's pitch (the Studio II has no other sound). */
void DebuggerWindow::refreshSound()
{
    studio2debug_io a;
    studio2debug_io_get(m_dbg, &a);
    static const char *const notes[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    QString s;
    s += QStringLiteral("Beeper (Q)   %1\n").arg(a.q ? QStringLiteral("on, sounding") : QStringLiteral("off, silent"));
    s += QStringLiteral("Pitch        %1\n").arg(a.beep_hz > 0 ? QStringLiteral("%1 Hz").arg(a.beep_hz) : QStringLiteral("-"));
    if (a.beep_hz > 0) {
        const double midi = 69.0 + 12.0 * std::log2(a.beep_hz / 440.0);
        const int note = (int)std::lround(midi);
        s += QStringLiteral("Nearest note %1%2 (%3 cents)\n").arg(QString::fromUtf8(notes[((note % 12) + 12) % 12]))
                 .arg(note / 12 - 1).arg((int)std::lround((midi - note) * 100.0), 0, 10);
    }
    s += QStringLiteral("\nThe Studio II's only sound is Q: one square wave, no volume.\n");
    setText(m_sound, s);
}

/* The labels the engine holds, filtered, in the order the filter gives
 * (address, then name). The table is rebuilt only when what it would show
 * changed: the engine's own index order is not stable, so the entries are
 * fetched anew each time and compared by content. */
void DebuggerWindow::refreshSymbols(bool force)
{
    const int total = studio2debug_symbols_count(m_dbg);
    const QByteArray filter = m_symFilter->text().trimmed().toUtf8();
    std::vector<int> indices((size_t)qMax(total, 0));
    const int shown = total > 0 ? studio2debug_symbols_filter(m_dbg, filter.constData(), indices.data(), total) : 0;

    struct Row { uint16_t addr; char name[96]; int bank; };
    std::vector<Row> rows;
    rows.reserve((size_t)shown);
    QString sig = QStringLiteral("%1/%2/%3:").arg(total).arg(shown).arg(QString::fromUtf8(filter));
    for (int i = 0; i < shown; ++i) {
        Row row = {};
        if (!studio2debug_symbols_get(m_dbg, indices[(size_t)i], &row.addr, row.name, (int)sizeof row.name, &row.bank)) continue;
        rows.push_back(row);
        sig += QStringLiteral("%1 %2 %3;").arg(row.addr).arg(row.bank).arg(QString::fromUtf8(row.name));
    }
    if (!force && sig == m_symSig) return;
    m_symSig = sig;

    m_symCount->setText(QStringLiteral("%1 of %2 symbols").arg(rows.size()).arg(total));
    const int top = m_symTable->verticalScrollBar()->value();
    m_symTable->setUpdatesEnabled(false);
    m_symTable->setRowCount((int)rows.size());
    for (int i = 0; i < (int)rows.size(); ++i) {
        auto *addr = new QTableWidgetItem(QStringLiteral("$") + hx(rows[(size_t)i].addr, 4));
        addr->setData(Qt::UserRole, rows[(size_t)i].addr);
        m_symTable->setItem(i, 0, addr);
        m_symTable->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(rows[(size_t)i].name)));
        m_symTable->setItem(i, 2, new QTableWidgetItem(rows[(size_t)i].bank >= 0 ? QString::number(rows[(size_t)i].bank)
                                                                                 : QString()));
    }
    m_symTable->setUpdatesEnabled(true);
    m_symTable->verticalScrollBar()->setValue(top);
}

void DebuggerWindow::addWatch(const QString &entry)
{
    const QByteArray text = entry.trimmed().toUtf8();
    if (text.isEmpty()) return;
    if (studio2watch_add(text.constData()) < 0) {
        message(QStringLiteral("The watch list is full"));
        return;
    }
    m_wtEntry->clear();
    refreshWatches(true);
}

/* Break on change: stop after a write over the watch that alters a byte. */
void DebuggerWindow::watchBreak(int index)
{
    uint16_t lo, hi;
    if (!studio2watch_range(m_dbg, index, &lo, &hi)) {
        message(QStringLiteral("That watch does not resolve yet"));
        return;
    }
    if (studio2debug_breakpoint_add(m_dbg, STUDIO2DEBUG_BP_WRITE, lo, hi, "value != old") < 0)
        message(QStringLiteral("The breakpoint could not be added"));
    else
        message(QStringLiteral("Breaking when it changes"));
    resync();
}

/* One row a watch, its value live (a dash while the machine runs). The rows
 * are rebuilt when the list changed; otherwise only their text moves. */
void DebuggerWindow::refreshWatches(bool force)
{
    const int n = studio2watch_count();
    const unsigned gen = studio2watch_generation();
    QFont mono = m_wtTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    if (force || gen != m_wtGen || m_wtTable->rowCount() != n) {
        m_wtGen = gen;
        m_wtTable->setRowCount(n);
        for (int i = 0; i < n; ++i) {
            auto *item = new QTableWidgetItem;
            item->setFont(mono);
            m_wtTable->setItem(i, 0, item);
            auto *box = new QWidget;
            auto *h = new QHBoxLayout(box);
            h->setContentsMargins(2, 0, 2, 0);
            auto *mem = new QPushButton(QStringLiteral("Memory"));
            mem->setToolTip(QStringLiteral("Show it in the Memory tab"));
            connect(mem, &QPushButton::clicked, this, [this, i] {
                uint16_t lo, hi;
                if (!studio2watch_range(m_dbg, i, &lo, &hi)) {
                    message(QStringLiteral("That watch does not resolve yet"));
                    return;
                }
                if (m_popped[TAB_MEMORY]) m_popped[TAB_MEMORY]->activateWindow();
                else m_tabs->setCurrentWidget(m_pages[TAB_MEMORY]);
                showInMemory(lo, hi);
            });
            auto *brk = new QPushButton(QStringLiteral("Break on change"));
            brk->setToolTip(QStringLiteral("Stop after a write that alters it"));
            connect(brk, &QPushButton::clicked, this, [this, i] { watchBreak(i); });
            auto *rm = new QPushButton(QStringLiteral("Remove"));
            connect(rm, &QPushButton::clicked, this, [this, i] {
                studio2watch_remove(i);
                QTimer::singleShot(0, this, [this] { refreshWatches(true); });
            });
            for (QPushButton *b : { mem, brk, rm }) {
                b->setFocusPolicy(Qt::NoFocus);
                h->addWidget(b);
            }
            m_wtTable->setCellWidget(i, 1, box);
        }
    }
    for (int i = 0; i < n; ++i) {
        char line[256];
        studio2watch_format(m_dbg, i, line, sizeof line);
        const QString text = stripControl(line);
        if (m_wtTable->item(i, 0)->text() != text) m_wtTable->item(i, 0)->setText(text);
    }
}

/* Trace (live) and History (at a stop): the ring newest first, shown
 * oldest first, the newest HIST_SHOWN lines that pass the filter. */
void DebuggerWindow::refreshHist(int idx, bool force)
{
    static studio2debug_trace buf[RING_MAX];
    const bool stopped = studio2debug_is_stopped(m_dbg) != 0;
    const bool on = studio2debug_trace_enabled(m_dbg) != 0;
    const int count = studio2debug_trace_count(m_dbg);

    m_hSync = true;                     /* both Record buttons follow the engine */
    for (int i = 0; i < 2; ++i)
        if (m_hRec[i]->isChecked() != on) m_hRec[i]->setChecked(on);
    m_hSync = false;

    if (force) m_hSeen[idx] = -1;
    /* History reads at a stop (it keeps what it showed while the machine
     * runs); at a stop neither view rebuilds for the same count */
    if (idx == 1 && !stopped && m_hSeen[1] >= 0) return;
    if (stopped && count == m_hSeen[idx]) return;
    m_hSeen[idx] = count;

    uint16_t lo = 0, hi = 0xFFFF;
    int filter = STUDIO2DEBUG_TRACE_ALL;
    switch (m_hFilter[idx]->currentIndex()) {
    case 1: filter = STUDIO2DEBUG_TRACE_BRANCHES; break;
    case 2:
        if (studio2sym_parse_range(m_dbg, m_hRange[idx]->text().toUtf8().constData(), &lo, &hi))
            filter = STUDIO2DEBUG_TRACE_RANGE;
        break;
    default: break;
    }
    const int n = studio2debug_trace_read(m_dbg, buf, RING_MAX);
    QList<int> hit;
    for (int i = 0; i < n && hit.size() < HIST_SHOWN; ++i)
        if (studio2debug_trace_match(&buf[i], filter, lo, hi)) hit.append(i);
    QString t;
    if (!on && n == 0) t = QStringLiteral("Recording is off. Press Record, then let the machine run.");
    char line[256];
    for (int k = (int)hit.size() - 1; k >= 0; --k) {
        studio2debug_trace_format(&buf[hit[k]], line, sizeof line);
        t += QString::fromUtf8(line);
        if (k) t += QLatin1Char('\n');
    }
    setText(m_hText[idx], t, true);
    m_hNote[idx]->setText(on ? QStringLiteral("recording, %1 instructions").arg(n)
                             : n ? QStringLiteral("recording off, %1 instructions").arg(n)
                                 : QStringLiteral("recording off"));
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
void DebuggerWindow::refreshFujinet(bool force)
{
    static studio2debug_fuji_packet p[TRACE_MAX];
    static char state[4096];
    const int n = studio2debug_fujinet_trace(m_dbg, p, TRACE_MAX);
    char status[128];
    studio2session_cart_status(m_session, status, sizeof status);
    if (studio2session_fujinet_external(m_session))
        m_fujiNote->setText(QStringLiteral("External FujiNet at %1. Cartridge: %2.")
                                .arg(QString::fromUtf8(studio2session_boip_endpoint(m_session)), QString::fromUtf8(status)));
    else if (!studio2session_fujinet_running(m_session))
        m_fujiNote->setText(QStringLiteral("FujiNet is off (Preferences ▸ FujiNet)."));
    else
        m_fujiNote->setText(QStringLiteral("FujiNet is on. Cartridge: %1.").arg(QString::fromUtf8(status)));
    state[0] = '\0';
    studio2debug_fujinet_state(m_dbg, state, int(sizeof state));
    setText(m_fujiState, stripControl(state));

    if (!force && n == m_fujiCount && (n == 0 || p[n - 1].serial == m_fujiLast)) return;
    const bool follow = force || m_fujiCount < 0 || atEnd(m_fujiText);
    m_fujiCount = n;
    m_fujiLast = n > 0 ? p[n - 1].serial : 0;
    QString t;
    for (int i = 0; i < n; ++i) {
        if (i) t += QLatin1Char('\n');
        t += QString::fromUtf8(p[i].text);
        if (p[i].repeat > 1) t += QStringLiteral(" ×%1").arg(p[i].repeat);
        t += QStringLiteral("\n     ");
        for (int k = 0; k < p[i].nbytes && k < int(sizeof p[i].data); k++) t += hx(p[i].data[k], 2) + QLatin1Char(' ');
        if (p[i].nbytes > int(sizeof p[i].data)) t += QStringLiteral("…");
    }
    if (n == 0) t = QStringLiteral("(no FujiBus packets since the trace was cleared)");
    setText(m_fujiText, t, follow);
}

/* One tab; live is the twice-a-second refresh while the machine runs, which
 * leaves out what only changes at a stop. */
void DebuggerWindow::refreshPage(int page, bool live)
{
    switch (page) {
    case TAB_DISASM: if (!live) refreshDisasm(); break;
    case TAB_MEMORY: refreshMem(); break;
    case TAB_VIDEO: refreshVideo(std::exchange(m_videoForce, false)); break;
    case TAB_BREAKS: refreshBps(); break;
    case TAB_WATCHES: refreshWatches(false); break;
    case TAB_CART: refreshCart(); break;
    case TAB_TRACE: refreshHist(0, false); break;
    case TAB_HISTORY: if (!live) refreshHist(1, false); break;
    case TAB_SOUND: refreshSound(); break;
    case TAB_SYMBOLS: refreshSymbols(false); break;
    case TAB_FUJINET: refreshFujinet(false); break;
    default: break;
    }
}

/* The registers show everywhere; then the tab showing and any in a window of
 * its own. */
void DebuggerWindow::refreshAll()
{
    refreshStatus();
    refreshCpu();
    refreshPage(currentTab(), false);
    for (int t = 0; t < TAB_COUNT; ++t)
        if (m_popped[t]) refreshPage(t, false);
}

/* A page's number, whichever place it holds in the tab widget now. */
int DebuggerWindow::currentTab() const
{
    QWidget *w = m_tabs->currentWidget();
    return w ? w->property("tab").toInt() : -1;
}

/* On screen: the tab widget's page, or a window of its own. */
bool DebuggerWindow::tabVisible(int tab) const
{
    return currentTab() == tab || m_popped[tab] != nullptr;
}

/* A new stop brings every view to the spot, whatever the reader had
 * scrolled to: the Disassembly to the PC, Memory to the range it marks (or
 * the PC), the Trace and History to their newest line, the Video tab to a
 * redraw. */
void DebuggerWindow::recentreOnStop()
{
    m_followPc->setChecked(true);
    m_disasm->verticalScrollBar()->setValue(0);
    m_hSeen[0] = m_hSeen[1] = -1;
    m_videoForce = true;
    refreshAll();
    studio2debug_cpu c;
    studio2debug_cpu_get(m_dbg, &c);
    scrollMemory(m_hl ? m_hlLo : (uint16_t)c.pc);
    for (QPlainTextEdit *view : m_hText)
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
}

/* After anything that may have moved the engine: the console notes a new
 * stop, then every view follows it. */
void DebuggerWindow::resync()
{
    const bool stopped = studio2debug_is_stopped(m_dbg) != 0;
    const uint64_t serial = studio2debug_stop_serial(m_dbg);
    const bool newStop = stopped && serial != m_seenSerial;
    if (newStop) {
        char reason[160];
        int addr;
        reason[0] = '\0';
        studio2debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
        /* a step already says where it is */
        if (reason[0] && strncmp(reason, "step", 4) != 0)
            consoleAppend(QStringLiteral("— %1\n").arg(stripControl(reason)));
    }
    m_seenSerial = serial;
    m_wasStopped = stopped;
    m_seenGen = studio2debug_generation(m_dbg);
    m_runningTicks = 0;
    if (newStop) recentreOnStop();
    else refreshAll();
}

/* From inside a breakpoint row's own signal: the table rebuilds after. */
void DebuggerWindow::syncLater()
{
    QTimer::singleShot(0, this, [this] { resync(); });
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
void DebuggerWindow::settleAndSync()
{
    for (int i = 0; i < 40 && !studio2debug_is_stopped(m_dbg); ++i) QThread::msleep(1);
    resync();
}

/* The engine reports a stop through the generation and the stop serial, so
 * this polls them: a refresh when they moved, and the live values twice a
 * second while the machine runs. */
void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    const unsigned gen = studio2debug_generation(m_dbg);
    const bool stopped = studio2debug_is_stopped(m_dbg) != 0;
    if (gen != m_seenGen || stopped != m_wasStopped || studio2debug_stop_serial(m_dbg) != m_seenSerial) {
        resync();
    } else if (tabVisible(TAB_WATCHES) && studio2watch_generation() != m_wtGen) {
        refreshWatches(false);          /* the list changed elsewhere (the console) */
    } else if (!stopped && ++m_runningTicks >= 4) {
        m_runningTicks = 0;
        refreshStatus();
        refreshCpu();
        refreshPage(currentTab(), true);
        for (int t = 0; t < TAB_COUNT; ++t)
            if (m_popped[t]) refreshPage(t, true);
    }
}

/* ---- the console ---------------------------------------------------------- */

/* Onto the transcript, which follows its end while the reader is there. */
void DebuggerWindow::consoleAppend(const QString &text)
{
    const bool follow = atEnd(m_conOut);
    QTextCursor cur(m_conOut->document());
    cur.movePosition(QTextCursor::End);
    cur.insertText(text);
    if (follow) m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
}

void DebuggerWindow::runConsole()
{
    static char out[65536];
    const QString cmd = m_conIn->text().trimmed();
    if (cmd.isEmpty()) return;
    if (m_history.isEmpty() || m_history.last() != cmd) m_history << cmd;
    m_historyPos = (int)m_history.size();
    /* a command brings the reader back down */
    m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
    consoleAppend(QStringLiteral("> %1\n").arg(cmd));
    m_conIn->clear();

    out[0] = '\0';
    studio2debug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    QString text = stripControl(out);
    if (!text.isEmpty()) consoleAppend(text.endsWith(QLatin1Char('\n')) ? text : text + QLatin1Char('\n'));
    settleAndSync();
}

/* Tab: the engine completes the word at the end of the line. One match
 * replaces it; several are listed and the line extends to what they share. */
void DebuggerWindow::completeConsole()
{
    char comps[4096];
    const QString text = m_conIn->text();
    comps[0] = '\0';
    const int n = studio2debug_completions(m_dbg, text.toUtf8().constData(), comps, sizeof comps);
    if (n <= 0) return;
    const QStringList words = stripControl(comps).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (words.isEmpty()) return;
    const int sp = text.lastIndexOf(QLatin1Char(' '));
    const QString head = text.left(sp + 1);
    if (words.size() == 1) {
        m_conIn->setText(head + words[0] + QLatin1Char(' '));
        return;
    }
    QString common = words[0];
    for (const QString &w : words)
        while (!w.startsWith(common)) common.chop(1);
    consoleAppend(words.join(QStringLiteral("  ")) + QLatin1Char('\n'));
    if (common.size() > text.size() - sp - 1) m_conIn->setText(head + common);
}

/* Messages land in the console and, since it may not be the tab showing,
 * the status bar. */
void DebuggerWindow::say(const QString &msg)
{
    consoleAppend(msg + QLatin1Char('\n'));
    message(msg);
}

void DebuggerWindow::message(const QString &msg)
{
    statusBar()->showMessage(msg, 6000);
}

/* ---- the rest of the handlers ----------------------------------------------- */

void DebuggerWindow::toggleRun()
{
    if (studio2debug_is_stopped(m_dbg)) {
        studio2debug_resume(m_dbg);
        resync();
    } else {
        studio2debug_stop(m_dbg);
        settleAndSync();
    }
}

void DebuggerWindow::stepAnd(const std::function<void()> &fn)
{
    if (!studio2debug_is_stopped(m_dbg)) {
        message(QStringLiteral("Stop the machine first"));
        return;
    }
    fn();
    settleAndSync();
}

void DebuggerWindow::runToCursor()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly to run to"));
        return;
    }
    stepAnd([this] { studio2debug_run_to(m_dbg, (uint16_t)m_selected); });
}

void DebuggerWindow::toggleBreakpoint(int addr)
{
    studio2debug_breakpoint_toggle(m_dbg, (uint16_t)addr);
    resync();
}

void DebuggerWindow::toggleSelected()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly first"));
        return;
    }
    toggleBreakpoint(m_selected);
}

/* The toolbar's Break at: a symbol or a hex address, an execute breakpoint
 * there (one already set stays). */
void DebuggerWindow::breakAt()
{
    const QString text = m_breakAt->text().trimmed();
    if (text.isEmpty()) return;
    long addr;
    QString why;
    if (!addressOf(text, &addr, &why)) {
        message(why);
        return;
    }
    if (studio2debug_breakpoint_check(m_dbg, (uint16_t)addr))
        message(QStringLiteral("A breakpoint is already set at $%1").arg(hx((unsigned)addr, 4)));
    else {
        studio2debug_breakpoint_toggle(m_dbg, (uint16_t)addr);
        message(QStringLiteral("Breakpoint at $%1").arg(hx((unsigned)addr, 4)));
    }
    m_breakAt->clear();
    resync();
}

/* A number typed into an entry: a label first, then bare digits as hex (as in
 * the sibling debuggers), then whatever the engine evaluates ($0900, 0x900,
 * 900h, r3+4, label+2; the console itself reads bare digits as decimal).
 * When it is none of those, the reason goes to *why, or to the status bar
 * without. */
bool DebuggerWindow::numberOf(const QString &text, long *out, QString *why)
{
    static const QRegularExpression hexOnly(QStringLiteral("^[0-9A-Fa-f]+$"));
    const QString t = text.trimmed();
    const QByteArray utf8 = t.toUtf8();
    long v = 0;
    QString reason;
    uint16_t lo, hi;
    const int label = studio2debug_label_address(m_dbg, utf8.constData());
    if (label >= 0) {
        v = label;
    } else if (hexOnly.match(t).hasMatch()) {
        bool ok;
        v = t.toLong(&ok, 16);
        if (!ok) reason = QStringLiteral("Not a number");
    } else if (studio2sym_parse_range(m_dbg, utf8.constData(), &lo, &hi) && lo == hi) {
        v = lo;
    } else {
        reason = QStringLiteral("Not a number ($0900, or a label): %1").arg(t);
    }
    if (reason.isEmpty()) {
        *out = v;
        return true;
    }
    if (why) *why = reason;
    else message(reason);
    return false;
}

/* The same, as an address: 0 to $FFFF. */
bool DebuggerWindow::addressOf(const QString &text, long *out, QString *why)
{
    long v;
    if (!numberOf(text, &v, why)) return false;
    if (v < 0 || v > 0xFFFF) {
        const QString reason = QStringLiteral("Out of range ($0000-$FFFF)");
        if (why) *why = reason;
        else message(reason);
        return false;
    }
    *out = v;
    return true;
}

/* A breakpoint's range from its two entries. The start may itself be a
 * range (lo-hi); the end is optional. An IN or OUT breakpoint is a port
 * (1-7). */
bool DebuggerWindow::rangeOf(const QString &startText, const QString &endText, int type, uint16_t *lo, uint16_t *hi,
                             QString *why)
{
    long first = 0, last = 0;
    const QString start = startText.trimmed(), end = endText.trimmed();
    const bool port = type == STUDIO2DEBUG_BP_IN || type == STUDIO2DEBUG_BP_OUT;
    if (end.isEmpty()) {
        uint16_t from, to;
        if (!studio2sym_parse_range(m_dbg, start.toUtf8().constData(), &from, &to)) {
            *why = QStringLiteral("Not an address, label or range: %1").arg(start);
            return false;
        }
        first = from;
        last = to;
    } else {
        if (!addressOf(start, &first, why)) return false;
        last = first;
        if (!addressOf(end, &last, why)) return false;
    }
    if (last < first) {
        *why = QStringLiteral("The end is before the start");
        return false;
    }
    if (port && (first < 1 || last > 7)) {
        *why = QStringLiteral("A port is 1-7");
        return false;
    }
    *lo = (uint16_t)first;
    *hi = (uint16_t)last;
    return true;
}

/* Edits go in only while the machine is stopped. A register entry shows hex
 * digits, so bare digits there are hex; anything else is an expression. */
void DebuggerWindow::setReg(int reg)
{
    QLineEdit *e = m_reg[reg];
    long v = 0;
    QString why;
    const bool ok = numberOf(e->text(), &v, &why);
    if (!studio2debug_is_stopped(m_dbg)) message(QStringLiteral("Stop the machine first"));
    else if (!ok) message(why);
    else studio2debug_cpu_set(m_dbg, reg, (int)v);
    e->clearFocus();
    resync();
}

/* Shows a range in the dump, from just above it, marked and scrolled to. */
void DebuggerWindow::showInMemory(uint16_t lo, uint16_t hi)
{
    m_hl = true;
    m_hlLo = lo;
    m_hlHi = hi;
    refreshMem();
    scrollMemory(lo);
}

/* Opens an address in the Disassembly (the tab, or its window), the PC no
 * longer followed, the line selected. */
void DebuggerWindow::showInDisassembly(uint16_t addr)
{
    if (m_popped[TAB_DISASM]) m_popped[TAB_DISASM]->activateWindow();
    else m_tabs->setCurrentWidget(m_pages[TAB_DISASM]);
    m_followPc->setChecked(false);
    m_selected = addr;
    m_disasmTop = (uint16_t)studio2debug_row_address(m_dbg, addr, -(visibleRows() / 3));
    refreshDisasm();
}

/* Go to: a label, an address, an expression or a range (lo-hi), which is
 * marked. Bare digits are hex, as everywhere in the entries. */
void DebuggerWindow::memGoto()
{
    const QString text = m_memGoto->text().trimmed();
    if (text.isEmpty()) return;
    uint16_t lo, hi;
    if (!studio2sym_parse_range(m_dbg, text.toUtf8().constData(), &lo, &hi)) {
        m_hl = false;
        refreshMem();
        message(QStringLiteral("Not an address, label or range: %1").arg(text));
        return;
    }
    showInMemory(lo, hi);
}

/* Find Next / Previous from the marked range (or the top of the view); the
 * hit is marked. */
void DebuggerWindow::memSearch(bool backward)
{
    uint8_t pat[STUDIO2SEARCH_MAX], mask = 0xFF;
    int n = 0, flags = 0;
    if (!studio2search_parse(m_srchKind->currentIndex(), m_srchEntry->text().toUtf8().constData(), pat, &n, &mask,
                         &flags) || n <= 0) {
        m_srchNote->setText(QStringLiteral("Type hex bytes (3E 5A) or some text."));
        return;
    }
    uint16_t from = 0, found = 0;
    if (m_hl) from = backward ? m_hlLo : (uint16_t)(m_hlLo + 1);
    const int r = studio2debug_search_next(m_dbg, pat, n, mask, flags, from, backward ? 1 : 0, &found);
    if (r < 0) {
        m_srchNote->setText(QStringLiteral("Stop the machine first"));
        message(QStringLiteral("Stop the machine first"));
    } else if (r == 0) {
        m_srchNote->setText(QStringLiteral("Not found"));
        message(QStringLiteral("Not found"));
    } else {
        m_srchNote->setText(QStringLiteral("Found at $%1").arg(hx(found, 4)));
        showInMemory(found, (uint16_t)(found + n - 1));
    }
}

void DebuggerWindow::memWrite()
{
    long a, v;
    if (!addressOf(m_memAddr->text(), &a)) return;
    if (!numberOf(m_memVal->text(), &v)) return;
    if (v < 0 || v > 0xFF) {
        message(QStringLiteral("The value is a byte ($00-$FF)"));
        return;
    }
    studio2debug_write(m_dbg, (uint16_t)a, (uint8_t)v);
    resync();
}

void DebuggerWindow::jumpTo()
{
    long a;
    if (!addressOf(m_jump->text(), &a)) return;
    m_followPc->setChecked(false);
    m_selected = (int)a;
    m_disasmTop = (uint16_t)studio2debug_row_address(m_dbg, (uint16_t)a, -(visibleRows() / 3));
    refreshDisasm();
}

/* The type takes an address, range or label; in and out a port; VRAM and
 * CRAM writes a VRAM address or a CRAM index. The end is optional; the
 * condition is an expression the engine checks. */
void DebuggerWindow::addBreakpoint()
{
    const int type = bpTypeAt(m_bpType->currentIndex());
    const QString start = m_bpStart->text().trimmed(), end = m_bpEnd->text().trimmed();
    if (start.isEmpty()) return;
    uint16_t lo = 0, hi = 0;
    QString why;
    if (!rangeOf(start, end, type, &lo, &hi, &why)) {
        m_bpMsg->setText(why);
        return;
    }
    const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
    if (studio2debug_breakpoint_add(m_dbg, type, lo, hi, cond.constData()) < 0) {
        m_bpMsg->setText(QStringLiteral("The condition does not parse"));
        return;
    }
    m_bpMsg->clear();
    m_bpStart->clear();
    m_bpEnd->clear();
    m_bpCond->clear();
    resync();
}

/* A symbol file the user picks, or (fromFile false) the <cart>.sym/.map/.noi
 * beside the opened cartridge. */
void DebuggerWindow::loadSymbols(bool fromFile)
{
    QString path;
    if (fromFile) {
        const QString cart = QString::fromUtf8(studio2session_cart_path(m_session));
        path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Load Symbols"), cart.isEmpty() ? QString() : QFileInfo(cart).absolutePath(),
            QStringLiteral("Symbol files (*.sym *.map *.noi *.txt);;All files (*)"));
        if (path.isEmpty()) return;
    }
    char msg[512] = "";
    const QByteArray p = path.toLocal8Bit();
    studio2debug_load_symbols(m_dbg, fromFile ? p.constData() : nullptr, msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("No symbols found"));
    refreshSymbols(true);
    resync();
}

void DebuggerWindow::clearSymbols()
{
    studio2debug_clear_symbols(m_dbg);
    say(QStringLiteral("Symbols cleared (the built-in ones stay)"));
    refreshSymbols(true);
    resync();
}

void DebuggerWindow::save(const char *kind, const QString &title, const QString &file)
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save %1").arg(title), file);
    if (path.isEmpty()) return;
    char msg[512] = "";
    studio2debug_save(m_dbg, kind, path.toLocal8Bit().constData(), msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("Saved %1").arg(path));
}

/* ---- tabs in windows of their own ----------------------------------------- */

namespace {
/* A tab's window: closing it docks the tab again. */
class PopWindow : public QWidget {
public:
    std::function<void()> onClose;
protected:
    void closeEvent(QCloseEvent *e) override
    {
        if (onClose) onClose();
        QWidget::closeEvent(e);
    }
};
} // namespace

void DebuggerWindow::popOut(int tab)
{
    if (tab < 0 || tab >= TAB_COUNT || m_popped[tab] || m_tabs->count() <= 1) return;     /* one tab always stays */
    QWidget *page = m_pages[tab];
    m_tabs->removeTab(m_tabs->indexOf(page));

    auto *win = new PopWindow;
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setObjectName(QStringLiteral("debugger-pop-%1").arg(QLatin1String(kTabs[tab].name)));
    win->setWindowTitle(QStringLiteral("Studio II Debugger - %1").arg(QLatin1String(kTabs[tab].label)));
    win->setProperty("studio2HelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[tab].help)));
    auto *layout = new QVBoxLayout(win);
    auto *bar = new QHBoxLayout;
    auto *dockBtn = new QPushButton(QStringLiteral("Dock"));
    dockBtn->setToolTip(QStringLiteral("Put this tab back in the debugger"));
    bar->addWidget(dockBtn);
    bar->addStretch(1);
    bar->addWidget(studio2HelpButton(QString()));
    layout->addLayout(bar);
    layout->addWidget(page, 1);
    page->show();
    win->resize(760, 520);
    m_popped[tab] = win;
    static_cast<PopWindow *>(win)->onClose = [this, tab] {
        if (!m_closing) dock(tab);
    };
    connect(dockBtn, &QPushButton::clicked, win, &QWidget::close);
    /* the stepping keys work in the window too */
    new QShortcut(QKeySequence(Qt::Key_F1), win, [win] { studio2HelpFor(win); });
    new QShortcut(QKeySequence(Qt::Key_F5), win, [this] { toggleRun(); });
    new QShortcut(QKeySequence(Qt::Key_F7), win, [this] { stepAnd([this] { studio2debug_step(m_dbg); }); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F7), win, [this] { stepAnd([this] { studio2debug_frame(m_dbg, 1); }); });
    new QShortcut(QKeySequence(Qt::Key_F8), win, [this] { stepAnd([this] { studio2debug_step_over(m_dbg); }); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F8), win, [this] { stepAnd([this] { studio2debug_step_out(m_dbg); }); });
    if (isVisible()) win->show();
    if (tab == TAB_VIDEO) m_videoForce = true;
    if (tab == TAB_HISTORY) m_hSeen[1] = -1;
    refreshPage(tab, false);
}

void DebuggerWindow::dock(int tab)
{
    QWidget *win = m_popped[tab];
    if (!win) return;
    QWidget *page = m_pages[tab];
    m_popped[tab] = nullptr;
    int pos = 0;
    for (int t = 0; t < tab; ++t)       /* back where it was in the order */
        if (!m_popped[t]) ++pos;
    page->setParent(nullptr);
    m_tabs->insertTab(pos, page, QString::fromUtf8(kTabs[tab].label).replace(QLatin1Char('&'), QLatin1String("&&")));
    m_tabs->setCurrentIndex(pos);
    if (tab == TAB_VIDEO) m_videoForce = true;
    if (tab == TAB_HISTORY) m_hSeen[1] = -1;
    refreshPage(tab, false);
}

/* ---- events --------------------------------------------------------------- */

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    /* Tab completes; Up and Down walk the history */
    if (obj == m_conIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab) {
            completeConsole();
            return true;
        }
        if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
            if (ke->key() == Qt::Key_Up && m_historyPos > 0) --m_historyPos;
            else if (ke->key() == Qt::Key_Down && m_historyPos < m_history.size()) ++m_historyPos;
            m_conIn->setText(m_historyPos < m_history.size() ? m_history.at(m_historyPos) : QString());
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        /* A click selects a line (Run to Cursor); a click on its left edge,
         * or a double-click, toggles its breakpoint. A label has no line of
         * its own, so there is nothing else to hit. */
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() != Qt::LeftButton) return true;
            const QTextCursor cur = m_disasm->cursorForPosition(me->position().toPoint());
            const int line = cur.blockNumber();
            if (line < 0 || line >= (int)m_lineAddr.size()) return true;
            const bool gutter = cur.positionInBlock() <= 2;
            const bool dbl = e->type() == QEvent::MouseButtonDblClick;
            m_selected = m_lineAddr[(size_t)line];
            if (gutter ? !dbl : dbl) toggleBreakpoint(m_lineAddr[(size_t)line]);
            else refreshDisasm();
            return true;
        }
        if (e->type() == QEvent::MouseButtonRelease || e->type() == QEvent::MouseMove) return true;
        if (e->type() == QEvent::Wheel) {
            m_wheel += static_cast<QWheelEvent *>(e)->angleDelta().y();
            const int rows = -m_wheel / 40;     /* a notch is three rows */
            if (rows != 0) {
                m_wheel += rows * 40;
                m_followPc->setChecked(false);
                m_disasmTop = (uint16_t)studio2debug_row_address(m_dbg, m_disasmTop, rows);
                refreshDisasm();
            }
            return true;
        }
        if (e->type() == QEvent::Resize && isVisible()) refreshDisasm();
    }
    return QMainWindow::eventFilter(obj, e);
}

/* These keys are the debugger's here, taken as plain key presses ahead of
 * any application-wide shortcut. */
bool DebuggerWindow::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(e))) {
        e->accept();
        return true;
    }
    return QMainWindow::event(e);
}

void DebuggerWindow::keyPressEvent(QKeyEvent *e)
{
    if (!isDebuggerKey(e)) {
        QMainWindow::keyPressEvent(e);
        return;
    }
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    /* held, the steps repeat; the others do not */
    if (e->isAutoRepeat() && (e->key() == Qt::Key_F5 || e->key() == Qt::Key_F9 || e->key() == Qt::Key_F12)) return;
    switch (e->key()) {
    case Qt::Key_F5: toggleRun(); break;
    case Qt::Key_F7: stepAnd(shift ? std::function<void()>([this] { studio2debug_frame(m_dbg, 1); })
                                   : std::function<void()>([this] { studio2debug_step(m_dbg); })); break;
    case Qt::Key_F8: stepAnd(shift ? std::function<void()>([this] { studio2debug_step_out(m_dbg); })
                                   : std::function<void()>([this] { studio2debug_step_over(m_dbg); })); break;
    case Qt::Key_F9: toggleSelected(); break;
    default: hide(); break;     /* F12 */
    }
}

/* Shown: the engine attaches, which stops the machine, as on every sibling.
 * The tabs have no size until the window is up, so the first refresh waits
 * for the event loop. A restore from the taskbar is not a show. */
void DebuggerWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    m_timer.start(100);
    if (e->spontaneous()) return;
    for (QWidget *win : m_popped)
        if (win) win->show();
    studio2debug_attach(m_dbg);
    static bool historyOnce = true;     /* STUDIO2_HISTORY: record from the first show */
    if (historyOnce && qEnvironmentVariableIsSet("STUDIO2_HISTORY")) studio2debug_trace_enable(m_dbg, 1);
    historyOnce = false;
    m_wasStopped = false;
    QTimer::singleShot(0, this, [this] {
        if (!isVisible()) return;
        if (m_tabs->currentIndex() == TAB_CONSOLE) m_conIn->setFocus();
        settleAndSync();
    });
}

/* Hidden by F12, the close button or the main window: the engine goes away
 * and the machine runs on. The close button is a plain close, which only
 * hides the window (it is not deleted, so it comes back as it was);
 * ignoring the close instead would make QApplication's quit, which closes
 * every window first, give up whenever it reached this one. A minimised
 * window's spontaneous hide is not a detach. */
void DebuggerWindow::hideEvent(QHideEvent *e)
{
    m_timer.stop();
    if (!e->spontaneous()) {
        for (QWidget *win : m_popped)
            if (win) win->hide();
        studio2debug_detach(m_dbg);
    }
    QMainWindow::hideEvent(e);
}
