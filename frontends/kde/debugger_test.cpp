/*
 * debugger_test -- the Debugger window (debugger/DebuggerWindow.cpp) driven
 * with synthetic events on the offscreen platform against a running Studio II
 * session (the CONFIG client, the stand-in BIOS, FujiNet off) whose console RAM holds
 * a hand-assembled loop written through the debugger: the tabs,
 * attach/detach with show and hide, F12, the stepping keys and buttons, the
 * console (transcript, Tab completion, history), the disassembly (the PC's
 * accent bar, click / double-click / F9 for a breakpoint, selection and Run
 * to Cursor, the wheel, the jump box), the register and memory entries, the
 * Breakpoints tab (every type), Watches, search, Trace and History, Symbols,
 * Break at, pop-out and dock, the Video tab, and F12 and the View menu from
 * the main window.
 *
 * DBG_TEST_SHOTS=<dir> also grabs a PNG of each tab (DBG_TEST_LIGHT=1: in the
 * light scheme).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QOpenGLContext>
#include <QTemporaryDir>
#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QMenu>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QToolButton>

#include "studio2debug.h"
#include "studio2memtools.h"
#include "studio2session.h"
#include "DisplayWidget.h"
#include "MainWindow.h"
#include "debugger/DebuggerWindow.h"

namespace {

/* The test subject, in console RAM at $0820 (interrupts off, P = 3):
 *   0820  LDI $08 / PHI R8 / LDI $40 / PLO R8       R8 = $0840
 *   0826  LDI $08 / PHI R4 / LDI $50 / PLO R4       R4 = $0850, the subroutine
 *   082C  SEP R4                                    the call
 *   082D  LDN R8                                    read $0841
 *   082E  ADI 1
 *   0830  STR R8                                    write $0841
 *   0831  BR $0820
 *   0850  INC R8 / SEP R3                           the subroutine, back to P = 3 */
const uint8_t kProg[] = {
    0xF8, 0x08, 0xB8, 0xF8, 0x40, 0xA8, 0xF8, 0x08, 0xB4, 0xF8, 0x50, 0xA4,
    0xD4, 0x08, 0xFC, 0x01, 0x58, 0x30, 0x20,
};
const uint8_t kSub[] = { 0x18, 0xD3 };
const int kStart = 0x0820, kCall = 0x082C, kAfter = 0x082D, kStore = 0x0830, kSubAt = 0x0850, kCounter = 0x0841;
/* Display on over console RAM, the way the BIOS does it: R1 = the interrupt routine, R2 the
 * stack, INP 1 turns the display on; the 1861's interrupt on lines 78-79 runs the routine at
 * $08E2, which points R0 back at $0900 before the DMA starts on line 80 and returns. */
const uint8_t kDisplayOn[] = {
    0xF8, 0x08, 0xB1, 0xF8, 0xE2, 0xA1, 0xF8, 0x08, 0xB2, 0xF8, 0xFF, 0xA2, 0xE2, 0x69, 0x30, 0xCE,
};
const uint8_t kInterrupt[] = {
    0x72, 0x70, 0x22, 0x78, 0x22, 0x52, 0xF8, 0x09, 0xB0, 0xF8, 0x00, 0xA0, 0x30, 0xE0,
};
const int kInterruptAt = 0x08E0;
const int kDisplayAt = 0x08C0;

/* The accent: the Studio II icon's tan. */
const QColor kAccent(0xC8, 0xB5, 0x8C);

/* With the machine stopped: the program in place, PC at its second LDI. */
void loadProgram(studio2debug *d)
{
    for (size_t i = 0; i < sizeof kProg; i++) studio2debug_write(d, (uint16_t)(kStart + i), kProg[i]);
    for (size_t i = 0; i < sizeof kSub; i++) studio2debug_write(d, (uint16_t)(kSubAt + i), kSub[i]);
    studio2debug_cpu_set(d, STUDIO2_REG_IE, 0);
    studio2debug_cpu_set(d, STUDIO2_REG_P, 3);
    studio2debug_cpu_set(d, STUDIO2_REG_X, 2);
    studio2debug_cpu_set(d, STUDIO2_REG_PC, 0x0823);
}

int pcOf(studio2debug *d)
{
    studio2debug_cpu c;
    studio2debug_cpu_get(d, &c);
    return c.pc;
}

int regOf(studio2debug *d, int n)
{
    studio2debug_cpu c;
    studio2debug_cpu_get(d, &c);
    return c.r[n];
}

} // namespace

class DebuggerTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    studio2session *m_s = nullptr;
    studio2debug *m_d = nullptr;
    QMainWindow *m_win = nullptr;
    QTabWidget *m_tabs = nullptr;
    QString m_shots;
    studio2session_start_opts m_opts = {};

    QPushButton *button(const QString &textStart)
    {
        for (QPushButton *b : m_win->findChildren<QPushButton *>())
            if (b->text().startsWith(textStart)) return b;
        return nullptr;
    }
    QLineEdit *entry(const QString &placeholderStart)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText().startsWith(placeholderStart)) return e;
        return nullptr;
    }
    QLineEdit *entryIs(const QString &placeholder)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText() == placeholder) return e;
        return nullptr;
    }
    QPlainTextEdit *disasmView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff) return v;
        return nullptr;
    }
    QPlainTextEdit *consoleView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->lineWrapMode() == QPlainTextEdit::WidgetWidth) return v;
        return nullptr;
    }
    QLabel *statusLabel()
    {
        for (QLabel *l : m_win->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped")) ||
                l->text() == QLatin1String("Running"))
                return l;
        return nullptr;
    }
    QCheckBox *check(const QString &text)
    {
        for (QCheckBox *c : m_win->findChildren<QCheckBox *>())
            if (c->text() == text) return c;
        return nullptr;
    }
    QTableWidget *bpTable() { return m_tabs->widget(3)->findChild<QTableWidget *>(); }

    void waitStopped()
    {
        QTRY_VERIFY_WITH_TIMEOUT(studio2debug_is_stopped(m_d), 3000);
        /* the window follows within its poll */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel() &&
                                     statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
    }
    void shot(const QString &name)
    {
        if (!m_shots.isEmpty()) m_win->grab().save(m_shots + QLatin1Char('/') + name + QStringLiteral(".png"));
    }

    /* The block of the disassembly that shows an address, or -1. */
    int lineOf(QPlainTextEdit *v, int addr)
    {
        const QString want = QStringLiteral("%1").arg(addr, 4, 16, QLatin1Char('0')).toUpper();
        int n = 0;
        for (QTextBlock b = v->document()->begin(); b.isValid(); b = b.next(), ++n)
            if (b.text().mid(2, 4) == want) return n;
        return -1;
    }
    QPoint lineAt(QPlainTextEdit *v, int line, int column)
    {
        QTextCursor c(v->document()->findBlockByNumber(line));
        c.setPosition(c.block().position() + column);
        const QRect r = v->cursorRect(c);
        return QPoint(r.left() + 1, r.center().y());
    }
    void clickLine(int addr, int column, bool dbl = false)
    {
        QPlainTextEdit *v = disasmView();
        const int line = lineOf(v, addr);
        QVERIFY2(line >= 0, qPrintable(QStringLiteral("line for %1").arg(addr, 0, 16)));
        const QPoint p = lineAt(v, line, column);
        if (dbl) QTest::mouseDClick(v->viewport(), Qt::LeftButton, {}, p);
        else QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, p);
    }
    void typeInto(QLineEdit *e, const QString &text)
    {
        e->clear();
        QTest::keyClicks(e, text);
        QTest::keyClick(e, Qt::Key_Return);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("SDL_AUDIODRIVER", "dummy");
        if (qEnvironmentVariableIsSet("DBG_TEST_SHOTS")) m_shots = qEnvironmentVariable("DBG_TEST_SHOTS");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (qEnvironmentVariableIsSet("DBG_TEST_LIGHT"))
            QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
        studio2session_paths paths = {};
        const QByteArray dir = m_dir.path().toUtf8();
        paths.config_dir = dir.constData();
        paths.data_dir = dir.constData();
        paths.fujinet_lib = "";
        m_s = studio2session_new(&paths);
        QVERIFY(m_s);
        m_d = studio2session_debugger(m_s);
        QVERIFY(m_d);

        studio2session_default_opts(m_s, &m_opts);
        m_opts.cart_path = nullptr;
        m_opts.use_bios = 0;
        m_opts.enable_fujinet = 0;
        m_opts.enable_audio = 0;
        m_opts.enable_gamepad = 0;
        QCOMPARE(studio2session_start(m_s, &m_opts), 0);

        /* the window opens on the tab STUDIO2_DEBUGGER_TAB names, and STUDIO2_WATCH,
         * STUDIO2_WATCH_BREAK and STUDIO2_MEM_GOTO are read at creation */
        qputenv("STUDIO2_DEBUGGER_TAB", "i/o");
        qputenv("STUDIO2_WATCH", "$0870, $0880-$088F");
        qputenv("STUDIO2_WATCH_BREAK", "$0890");
        qputenv("STUDIO2_MEM_GOTO", "$08A0-$08AF");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("STUDIO2_DEBUGGER_TAB");
        qunsetenv("STUDIO2_WATCH");
        qunsetenv("STUDIO2_WATCH_BREAK");
        qunsetenv("STUDIO2_MEM_GOTO");
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->objectName() == QLatin1String("debugger")) m_win = qobject_cast<QMainWindow *>(w);
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
        QCOMPARE(m_tabs->currentIndex(), 5);
        QCOMPARE(studio2watch_count(), 3);                      /* two watches and the break-on-change one */
        studio2debug_breakpoint list[4];
        QCOMPARE(studio2debug_breakpoint_list(m_d, list, 4), 1);
        QCOMPARE((int)list[0].start, 0x0890);
        QVERIFY(QString::fromUtf8(list[0].condition).contains(QStringLiteral("old")));
        studio2watch_clear();
        m_tabs->setCurrentIndex(0);
        waitStopped();
    }

    void cleanupTestCase()
    {
        delete m_win;       /* detaches */
        if (m_s) {
            studio2session_stop(m_s);
            studio2session_free(m_s);
        }
    }

    void init()
    {
        if (!m_win->isVisible()) DebuggerWindow::toggleFor(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(studio2debug_is_attached(m_d), 2000);
        if (!studio2debug_is_stopped(m_d)) studio2debug_stop(m_d);
        waitStopped();
        studio2debug_breakpoint_clear(m_d);
        loadProgram(m_d);
    }

    void tabsAndAttach()
    {
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names, (QStringList{ QStringLiteral("Console"), QStringLiteral("Disassembly"),
                                      QStringLiteral("Memory"), QStringLiteral("Breakpoints"),
                                      QStringLiteral("Watches"), QStringLiteral("Cart && I/O"),
                                      QStringLiteral("Trace"), QStringLiteral("History"),
                                      QStringLiteral("Video"), QStringLiteral("Sound"),
                                      QStringLiteral("Symbols"), QStringLiteral("FujiNet") }));
        QVERIFY(studio2debug_is_attached(m_d));
        QVERIFY(studio2debug_is_stopped(m_d));
        /* Help's context follows the tab: the twelve names of the Help's debugger section */
        const char *const contexts[] = { "console", "disassembly", "memory", "breakpoints", "watches", "cart",
                                         "trace", "history", "video", "sound", "symbols", "fujinet" };
        for (int i = 0; i < 12; ++i) {
            m_tabs->setCurrentIndex(i);
            QCOMPARE(m_win->property("studio2HelpContext").toString(),
                     QStringLiteral("debugger/") + QLatin1String(contexts[i]));
        }
        m_tabs->setCurrentIndex(0);
        /* no tab sets the window's minimum height beyond a laptop screen */
        QVERIFY2(m_win->minimumSizeHint().height() <= 800, qPrintable(QString::number(m_win->minimumSizeHint().height())));
        /* Run is the accent while stopped, whichever text it wears, in one size */
        QPushButton *run = button(QStringLiteral("Run (F5)"));
        QVERIFY(run);
        QVERIFY(run->isChecked());
        QVERIFY(run->styleSheet().contains(kAccent.name()));
        const QSize size = run->size();
        QTest::mouseClick(run, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!studio2debug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(run->text() == QLatin1String("Stop (F5)"), 2000);
        QVERIFY(!run->isChecked());
        QCOMPARE(run->size(), size);
        QCOMPARE(statusLabel()->text(), QStringLiteral("Running"));
        QTest::mouseClick(run, Qt::LeftButton);
        waitStopped();
        QVERIFY(run->isChecked());
        QCOMPARE(run->size(), size);
        /* the toolbar has the family's buttons */
        for (const char *label : { "Step (F7)", "Step Over", "Step Out", "Scanline+1", "Frame+1", "Clear Breakpoints",
                                   "Load Symbols", "Clear Symbols", "Save", "Pop Out" })
            QVERIFY2(button(QString::fromUtf8(label)), label);
    }

    void stepKeys()
    {
        QCOMPARE(pcOf(m_d), 0x0823);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x0825, 2000);
        QCOMPARE(statusLabel()->text().left(13), QStringLiteral("Stopped: step"));
        /* F8 steps over the SEP R4 call: run to the instruction after it */
        studio2debug_cpu_set(m_d, STUDIO2_REG_PC, kCall);
        studio2debug_cpu_set(m_d, STUDIO2_REG_R0 + 4, kSubAt);
        QTest::keyClick(m_win, Qt::Key_F8);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), kAfter, 2000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("step over")));
        /* the button steps out of the subroutine: to the SEP R3 that returns */
        studio2debug_cpu_set(m_d, STUDIO2_REG_P, 4);
        studio2debug_cpu_set(m_d, STUDIO2_REG_R0 + 3, kAfter);
        studio2debug_cpu_set(m_d, STUDIO2_REG_PC, kSubAt);
        QTest::mouseClick(button(QStringLiteral("Step Out")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), kAfter, 2000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("step out")));
        /* Frame: Shift+F7 lands on a frame boundary */
        studio2debug_cpu c, c2;
        studio2debug_cpu_get(m_d, &c);
        QTest::keyClick(m_win, Qt::Key_F7, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(studio2debug_is_stopped(m_d) &&
                                     (studio2debug_cpu_get(m_d, &c2), c2.frame > c.frame), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("frame")), 2000);
        /* Scanline+1 */
        studio2debug_cpu_get(m_d, &c);
        QTest::mouseClick(button(QStringLiteral("Scanline+1")), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(studio2debug_is_stopped(m_d) &&
                                     (studio2debug_cpu_get(m_d, &c2), c2.vpos != c.vpos || c2.frame != c.frame), 3000);
        /* stepping while running is refused with a word, not silently */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!studio2debug_is_stopped(m_d), 2000);
        QTest::keyClick(m_win, Qt::Key_F7);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
    }

    /* The stop reason is the status line, and it is never cut: the label has
     * the width its whole text needs at the default window width. */
    void stopReasonNotElided()
    {
        studio2debug_breakpoint_add(m_d, STUDIO2DEBUG_BP_WRITE, kCounter, kCounter, "value != old");
        studio2debug_write(m_d, kCounter, 0x77);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(studio2debug_is_stopped(m_d), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("wrote $0841")), 2000);
        QLabel *status = statusLabel();
        QVERIFY(status->text().contains(QLatin1String("(was $77)")));
        QVERIFY2(status->width() >= status->fontMetrics().horizontalAdvance(status->text()),
                 qPrintable(QStringLiteral("status %1 wide for %2").arg(status->width())
                                .arg(status->fontMetrics().horizontalAdvance(status->text()))));
        QVERIFY(!status->text().contains(QChar(0x2026)));
        shot(QStringLiteral("stopreason"));
    }

    void consoleCommands()
    {
        m_tabs->setCurrentIndex(0);
        QLineEdit *in = entry(QStringLiteral("command"));
        QPlainTextEdit *out = consoleView();
        QVERIFY(in && out);
        QCOMPARE(m_win->focusWidget(), in);         /* typing goes straight in */
        typeInto(in, QStringLiteral("regs"));
        QVERIFY(out->toPlainText().contains(QStringLiteral("> regs")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("R8")));
        QVERIFY(in->text().isEmpty());
        /* a command that moves the machine moves the window */
        typeInto(in, QStringLiteral("set pc $0826"));
        QCOMPARE(pcOf(m_d), 0x0826);
        typeInto(in, QStringLiteral("break 2092"));     /* decimal, the engine's rule: $082C */
        QVERIFY(studio2debug_breakpoint_check(m_d, kCall));
        typeInto(in, QStringLiteral("run"));
        QTRY_VERIFY_WITH_TIMEOUT(out->toPlainText().contains(QStringLiteral("— breakpoint: exec $082C")), 3000);
        waitStopped();
        QCOMPARE(pcOf(m_d), kCall);
        QVERIFY(statusLabel()->text().contains(QStringLiteral("breakpoint: exec $082C")));
        /* Tab completes a command, lists several, and Up recalls */
        in->clear();
        QTest::keyClicks(in, QStringLiteral("reg"));
        QTest::keyClick(in, Qt::Key_Tab);
        QCOMPARE(in->text(), QStringLiteral("regs "));
        in->clear();
        QTest::keyClicks(in, QStringLiteral("b"));
        QTest::keyClick(in, Qt::Key_Tab);
        QVERIFY(out->toPlainText().contains(QStringLiteral("break")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("bpw")));
        QVERIFY(in->text().startsWith(QLatin1Char('b')));
        in->clear();
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("run"));
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("break 2092"));
        QTest::keyClick(in, Qt::Key_Down);
        QCOMPARE(in->text(), QStringLiteral("run"));
        in->clear();
        shot(QStringLiteral("console"));
    }

    /* Load Symbols > Next to the Cartridge (no cartridge file: said, not
     * silent), a symbol file loaded through the engine shows in the views, and
     * Break at takes a symbol or a hex address. */
    void symbolsAndBreakAt()
    {
        m_tabs->setCurrentIndex(0);
        QAction *next = nullptr;
        for (QAction *a : m_win->findChildren<QAction *>())
            if (a->text() == QLatin1String("Next to the Cartridge")) next = a;
        QVERIFY(next);
        const int before = consoleView()->document()->blockCount();
        next->trigger();
        QVERIFY(consoleView()->document()->blockCount() > before);
        QCOMPARE(studio2debug_label_address(m_d, "afterwards"), -1);
        QFile sym(m_dir.path() + QStringLiteral("/loop.sym"));
        QVERIFY(sym.open(QIODevice::WriteOnly));
        sym.write("082D afterwards\n0850 helper\n");
        sym.close();
        QLineEdit *in = entry(QStringLiteral("command"));
        typeInto(in, QStringLiteral("labels ") + sym.fileName());
        QCOMPARE(studio2debug_label_address(m_d, "afterwards"), kAfter);

        /* Break at: a symbol, then a hex address, then a repeat */
        QLineEdit *breakAt = entry(QStringLiteral("symbol or"));
        QVERIFY(breakAt);
        typeInto(breakAt, QStringLiteral("afterwards"));
        QVERIFY(studio2debug_breakpoint_check(m_d, kAfter));
        QVERIFY(breakAt->text().isEmpty());
        typeInto(breakAt, QStringLiteral("082E"));
        QVERIFY(studio2debug_breakpoint_check(m_d, 0x082E));
        typeInto(breakAt, QStringLiteral("082E"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("already")));
        studio2debug_breakpoint list[4];
        QCOMPARE(studio2debug_breakpoint_list(m_d, list, 4), 2);
        typeInto(breakAt, QStringLiteral("nosuchsymbol"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Not a number")));
        studio2debug_breakpoint_clear(m_d);

        /* the stop reason names where it is */
        typeInto(in, QStringLiteral("break afterwards"));
        QVERIFY(studio2debug_breakpoint_check(m_d, kAfter));
        typeInto(in, QStringLiteral("run"));
        QTRY_VERIFY_WITH_TIMEOUT(studio2debug_is_stopped(m_d) && pcOf(m_d) == kAfter, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("(afterwards)")), 2000);

        /* the Symbols tab lists them, filtered; activating a row opens the Disassembly there */
        m_tabs->setCurrentIndex(10);
        QTableWidget *table = m_tabs->currentWidget()->findChild<QTableWidget *>();
        QLineEdit *filter = m_tabs->currentWidget()->findChild<QLineEdit *>();
        QVERIFY(table && filter);
        QTRY_VERIFY_WITH_TIMEOUT(table->rowCount() >= 2, 2000);
        shot(QStringLiteral("symbols"));
        const int all = table->rowCount();
        filter->setText(QStringLiteral("AFTERW"));
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 2000);
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("afterwards"));
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("$082D"));
        table->setCurrentCell(0, 0);
        QTest::keyClick(table, Qt::Key_Return);             /* activating a row */
        QCOMPARE(m_tabs->currentIndex(), 1);
        QVERIFY(!check(QStringLiteral("Follow PC"))->isChecked());
        QVERIFY(lineOf(disasmView(), kAfter) >= 0);
        m_tabs->setCurrentIndex(10);
        filter->clear();
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), all, 2000);

        /* Clear Symbols drops the loaded ones */
        QTest::mouseClick(button(QStringLiteral("Clear Symbols")), Qt::LeftButton);
        QCOMPARE(studio2debug_label_address(m_d, "afterwards"), -1);
        QTRY_VERIFY_WITH_TIMEOUT(table->rowCount() < all, 2000);
        m_tabs->setCurrentIndex(0);
    }

    void disassemblyHighlights()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QVERIFY(v);
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        if (!follow->isChecked()) QTest::mouseClick(follow, Qt::LeftButton, {}, QPoint(4, follow->height() / 2));
        QVERIFY(follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x0823) >= 0, 2000);
        const int pcLine = lineOf(v, 0x0823);
        QCOMPARE(v->document()->findBlockByNumber(pcLine).text().left(2), QStringLiteral(" >"));
        /* follow-PC keeps it about a third of the way down */
        const int rows = v->document()->blockCount();
        QVERIFY2(pcLine >= rows / 3 - 3 && pcLine <= rows / 3 + 3, qPrintable(QStringLiteral("pc line %1 of %2").arg(pcLine).arg(rows)));
        /* the PC's line: the accent as the block's own background, so it lies under the gutter */
        const QTextBlock pcBlock = v->document()->findBlockByNumber(pcLine);
        QCOMPARE(pcBlock.blockFormat().background().color(), kAccent);
        for (const auto &s : v->extraSelections())
            QVERIFY(!s.format.boolProperty(QTextFormat::FullWidthSelection));
        QCOMPARE(v->document()->findBlockByNumber(pcLine + 1).blockFormat().background().style(), Qt::NoBrush);

        /* a breakpoint's dot: filled and red, hollow and grey when disabled */
        studio2debug_breakpoint_toggle(m_d, kCall);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, kCall)).text().startsWith(
                                     QStringLiteral("●")), 2000);
        auto dotColor = [&](int line) {
            QColor c;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == line && s.cursor.hasSelection() &&
                    s.cursor.selectionStart() == s.cursor.block().position() &&
                    s.cursor.selectionEnd() == s.cursor.block().position() + 1)
                    c = s.format.foreground().color();
            return c;
        };
        QCOMPARE(dotColor(lineOf(v, kCall)), QColor(0xe0, 0x1b, 0x24));
        studio2debug_breakpoint b;
        QCOMPARE(studio2debug_breakpoint_list(m_d, &b, 1), 1);
        studio2debug_breakpoint_enable(m_d, b.id, 0);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, kCall)).text().startsWith(
                                     QStringLiteral("○")), 2000);
        QCOMPARE(dotColor(lineOf(v, kCall)), QColor(0x9a, 0x99, 0x96));
        /* a breakpoint on the PC's own line keeps its dot, dark red on the tan bar */
        studio2debug_breakpoint_toggle(m_d, 0x0823);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, 0x0823)).text().startsWith(
                                     QStringLiteral("●")), 2000);
        QCOMPARE(dotColor(lineOf(v, 0x0823)), QColor(0xa0, 0x10, 0x18));
        /* the PC's line is dark ink on the pale tan */
        bool ink = false;
        QTextCursor cur(v->document()->findBlockByNumber(lineOf(v, 0x0823)));
        cur.setPosition(cur.block().position() + 10);
        ink = cur.charFormat().foreground().color() == QColor(Qt::black);
        QVERIFY(ink);
        shot(QStringLiteral("disassembly"));
    }

    void clickSelectsAndGutterToggles()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kAfter) >= 0, 2000);
        /* a click on the text selects (underlined, bold) and sets no breakpoint */
        clickLine(kAfter, 30);
        QVERIFY(!studio2debug_breakpoint_check(m_d, kAfter));
        {
            bool underlined = false;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == lineOf(v, kAfter) && s.format.fontUnderline() &&
                    s.format.fontWeight() == QFont::Bold)
                    underlined = true;
            QVERIFY(underlined);
        }
        /* moving to another line moves the selection; no caret-toggled breakpoints */
        clickLine(0x082E, 30);
        QVERIFY(!studio2debug_breakpoint_check(m_d, kAfter));
        QVERIFY(!studio2debug_breakpoint_check(m_d, 0x082E));
        /* the left edge toggles, a double-click toggles, F9 toggles the selection */
        clickLine(kStore, 0);
        QVERIFY(studio2debug_breakpoint_check(m_d, kStore));
        clickLine(kStore, 0);
        QVERIFY(!studio2debug_breakpoint_check(m_d, kStore));
        clickLine(0x0831, 30, true);
        QVERIFY(studio2debug_breakpoint_check(m_d, 0x0831));
        studio2debug_breakpoint_toggle(m_d, 0x0831);
        clickLine(kCall, 30);
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(studio2debug_breakpoint_check(m_d, kCall));
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(!studio2debug_breakpoint_check(m_d, kCall));
        studio2debug_breakpoint list[4];
        QCOMPARE(studio2debug_breakpoint_list(m_d, list, 4), 0);
    }

    void runToCursor()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kStore) >= 0, 2000);
        clickLine(kStore, 30);
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), kStore, 3000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("run to")));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kStore) >= 0, 2000);
    }

    void wheelJumpAndFollow()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QVERIFY(follow && follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x0823) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -120), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        QVERIFY(lineOf(v, 0x0823) < 0 || lineOf(v, 0x0823) < v->document()->blockCount() / 3 - 1);
        /* the jump box: bare digits are hex here, a label wins first, then expressions */
        QLineEdit *jump = entry(QStringLiteral("$address"));
        QVERIFY(jump);
        typeInto(jump, QStringLiteral("0850"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kSubAt) >= 0, 2000);
        studio2debug_set_label(m_d, kAfter, "back");
        typeInto(jump, QStringLiteral("back"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kAfter) >= 0, 2000);
        QVERIFY(v->toPlainText().contains(QStringLiteral("back")));
        typeInto(jump, QStringLiteral("$0850+1"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x0851) >= 0, 2000);
        typeInto(jump, QStringLiteral("nosuchlabel"));
        QVERIFY(!m_win->statusBar()->currentMessage().isEmpty());
        /* Follow PC again returns to the PC */
        QTest::mouseClick(follow, Qt::LeftButton, {}, QPoint(4, follow->height() / 2));
        QVERIFY(follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x0823) >= 0, 2000);
        studio2debug_set_label(m_d, kAfter, nullptr);
    }

    void registersAndMemory()
    {
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QList<QLineEdit *> regs;
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->toolTip().contains(QLatin1String("Enter sets it"))) regs << e;
        QCOMPARE(regs.size(), (int)STUDIO2_REG_COUNT);  /* R0-RF, PC, D, DF, P, X, T, B, Q, IE */
        QLineEdit *r5 = regs[5], *pc = regs[16], *dreg = regs[17], *df = regs[18];
        QTRY_COMPARE_WITH_TIMEOUT(pc->text(), QStringLiteral("0823"), 2000);
        typeInto(pc, QStringLiteral("082E"));
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x082E, 2000);
        typeInto(r5, QStringLiteral("1234"));
        QCOMPARE(regOf(m_d, 5), 0x1234);
        typeInto(r5, QStringLiteral("$0850+2"));        /* an expression */
        QCOMPARE(regOf(m_d, 5), 0x0852);
        typeInto(dreg, QStringLiteral("5a"));
        studio2debug_cpu c;
        studio2debug_cpu_get(m_d, &c);
        QCOMPARE(c.d, 0x5A);
        QTRY_COMPARE_WITH_TIMEOUT(dreg->text(), QStringLiteral("5A"), 2000);
        typeInto(df, QStringLiteral("1"));
        studio2debug_cpu_get(m_d, &c);
        QCOMPARE(c.df, 1);
        studio2debug_cpu_set(m_d, STUDIO2_REG_DF, 0);
        studio2debug_cpu_set(m_d, STUDIO2_REG_D, 0);
        /* EF1-EF4 and the next opcode's I:N are shown, not set; the state line is there */
        for (const char *name : { "EF1", "EF2", "EF3", "EF4" }) {
            QCheckBox *box = check(QString::fromUtf8(name));
            QVERIFY(box);
            QVERIFY(box->testAttribute(Qt::WA_TransparentForMouseEvents));
        }
        bool beam = false, opcode = false;
        studio2debug_write(m_d, 0x082E, 0xFC);
        studio2debug_cpu_set(m_d, STUDIO2_REG_PC, 0x082E);
        for (QLabel *l : m_win->findChildren<QLabel *>()) {
            if (l->text().contains(QLatin1String("clocks"))) beam = true;
            if (l->text().startsWith(QLatin1String("I:N"))) opcode = true;
        }
        QVERIFY(beam && opcode);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (QLabel *l : m_win->findChildren<QLabel *>())
                if (l->text().startsWith(QLatin1String("I:N"))) return l->text() == QLatin1String("I:N = F:C");
            return false;
        }(), 2000);
        /* P and X: bold and underlined labels */
        studio2debug_cpu_set(m_d, STUDIO2_REG_X, 2);

        /* memory: poke through the map, go to a label or an address */
        QLineEdit *addr = entryIs(QStringLiteral("$0900"));
        QLineEdit *val = entryIs(QStringLiteral("$00"));
        QVERIFY(addr && val);
        addr->setText(QStringLiteral("0870"));       /* hex */
        typeInto(val, QStringLiteral("5a"));
        uint8_t b = 0;
        studio2debug_read(m_d, 0x0870, &b, 1);
        QCOMPARE((int)b, 0x5A);
        addr->setText(QStringLiteral("$0871"));
        typeInto(val, QStringLiteral("$A5"));
        studio2debug_read(m_d, 0x0871, &b, 1);
        QCOMPARE((int)b, 0xA5);
        studio2debug_write(m_d, 0x0872, 0x11);
        addr->setText(QStringLiteral("0872"));
        typeInto(val, QStringLiteral("1FF"));        /* not a byte */
        studio2debug_read(m_d, 0x0872, &b, 1);
        QCOMPARE((int)b, 0x11);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("byte")));
        QPlainTextEdit *mem = memView();
        QLineEdit *go = entryIs(QStringLiteral("$0900, label or lo-hi"));
        QVERIFY(go && mem);
        typeInto(go, QStringLiteral("0820"));
        /* all 64K are there: the first row and the last, each named by its region */
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$0000: ")));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$FFF0: ")));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$0820: F8 08 B8 F8 40 A8")));
        QCOMPARE(mem->document()->blockCount(), 4097);
        const QString first = mem->document()->findBlockByNumber(1).text();
        const QString ram = mem->document()->findBlockByNumber(1 + 0x0820 / 16).text();
        QVERIFY2(!first.mid(72).trimmed().isEmpty(), qPrintable(first));
        QVERIFY2(ram.contains(QLatin1String("RAM")), qPrintable(ram));
        /* Go to scrolled the row into view and marked it in the accent, dark ink */
        QVERIFY(mem->verticalScrollBar()->value() >= 0x0820 / 16 - 40);
        bool marked = false;
        for (const auto &s : mem->extraSelections())
            marked |= s.format.background().color() == kAccent && s.format.foreground().color() == QColor(Qt::black);
        QVERIFY(marked);
        /* edits need a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!studio2debug_is_stopped(m_d), 2000);
        typeInto(pc, QStringLiteral("0820"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("memory"));
    }

    void breakpointsTab()
    {
        m_tabs->setCurrentIndex(3);
        QTableWidget *t = bpTable();
        QVERIFY(t);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QCOMPARE(t->rowCount(), 0);
        QLineEdit *start = entry(QStringLiteral("address, range"));
        QLineEdit *end = entry(QStringLiteral("end"));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(start && end && cond);
        QComboBox *type = m_tabs->currentWidget()->findChild<QComboBox *>();
        QVERIFY(type);
        QPushButton *add = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Add")) add = b;
        QPushButton *clear = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear Breakpoints")) clear = b;
        QLabel *msg = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("e01b24"))) msg = l;
        QVERIFY(add && clear && msg);

        QStringList types;
        for (int i = 0; i < type->count(); ++i) types << type->itemText(i);
        QCOMPARE(types, (QStringList{ QStringLiteral("Execute"), QStringLiteral("Read"), QStringLiteral("Write"),
                                      QStringLiteral("Access"), QStringLiteral("Port IN"), QStringLiteral("Port OUT") }));

        /* exec at a hex address */
        typeInto(start, QStringLiteral("082C"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QVERIFY2(t->item(0, 1)->text().startsWith(QLatin1String("Exec  $082C")), qPrintable(t->item(0, 1)->text()));
        QCOMPARE(t->item(0, 0)->checkState(), Qt::Checked);
        QVERIFY(start->text().isEmpty());
        /* a write watchpoint over a range, with a condition */
        type->setCurrentText(QStringLiteral("Write"));
        start->setText(QStringLiteral("0840"));
        end->setText(QStringLiteral("$084F"));
        cond->setText(QStringLiteral("value > 0"));
        QTest::mouseClick(add, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        QVERIFY2(t->item(1, 1)->text().startsWith(QLatin1String("Write  $0840-$084F")), qPrintable(t->item(1, 1)->text()));
        QVERIFY(t->item(1, 1)->text().contains(QLatin1String("value > 0")));
        /* the start may itself be a range */
        cond->clear();
        end->clear();
        type->setCurrentText(QStringLiteral("Access"));
        typeInto(start, QStringLiteral("0900-090F"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 3, 2000);
        QVERIFY2(t->item(2, 1)->text().startsWith(QLatin1String("Access  $0900-$090F")), qPrintable(t->item(2, 1)->text()));
        /* the port types */
        type->setCurrentText(QStringLiteral("Port OUT"));
        typeInto(start, QStringLiteral("2"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 4, 2000);
        QVERIFY2(t->item(3, 1)->text().contains(QLatin1String("2")), qPrintable(t->item(3, 1)->text()));
        type->setCurrentText(QStringLiteral("Port IN"));
        typeInto(start, QStringLiteral("1"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 5, 2000);
        /* the ranges are checked: a port is 1-7 */
        const int rowsBefore = t->rowCount();
        typeInto(start, QStringLiteral("8"));
        QVERIFY2(msg->text().contains(QLatin1String("1-7")), qPrintable(msg->text()));
        typeInto(start, QStringLiteral("0"));
        QVERIFY2(msg->text().contains(QLatin1String("1-7")), qPrintable(msg->text()));
        QCOMPARE(t->rowCount(), rowsBefore);
        /* errors are shown, not swallowed */
        type->setCurrentText(QStringLiteral("Execute"));
        cond->setText(QStringLiteral("d == (1 +"));
        start->setText(QStringLiteral("7000"));
        end->clear();
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(t->rowCount(), rowsBefore);
        cond->clear();
        type->setCurrentText(QStringLiteral("Port IN"));
        start->setText(QStringLiteral("7"));
        end->setText(QStringLiteral("5"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(msg->text().contains(QLatin1String("before")));
        start->clear();
        end->clear();
        msg->clear();
        type->setCurrentText(QStringLiteral("Execute"));
        shot(QStringLiteral("breakpoints-types"));

        /* back to two: the exec one and the conditional write */
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        typeInto(start, QStringLiteral("082D"));
        type->setCurrentText(QStringLiteral("Write"));
        start->setText(QStringLiteral("0840"));
        end->setText(QStringLiteral("$084F"));
        cond->setText(QStringLiteral("value > 0"));
        QTest::mouseClick(add, Qt::LeftButton);
        cond->clear();
        end->clear();
        type->setCurrentText(QStringLiteral("Execute"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);

        /* the exec breakpoint stops the run, with its hits counted */
        studio2debug_write(m_d, kCounter, 0);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(studio2debug_is_stopped(m_d) && pcOf(m_d) == kAfter, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("exec $082D")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 2) && t->item(0, 2)->text().toInt() >= 1, 2000);
        /* unticking it lets the run pass; the write watch then stops it instead */
        t->item(0, 0)->setCheckState(Qt::Unchecked);
        studio2debug_breakpoint list[2];
        QCOMPARE(studio2debug_breakpoint_list(m_d, list, 2), 2);
        QCOMPARE(list[0].enabled, 0);
        QCOMPARE(list[1].enabled, 1);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(studio2debug_is_stopped(m_d) && statusLabel()->text().contains(QLatin1String("wrote $0841")),
                                 4000);
        /* Clear Hits zeroes the counts */
        QPushButton *clearHits = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear Hits")) clearHits = b;
        QVERIFY(clearHits);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(1, 2) && t->item(1, 2)->text().toInt() >= 1, 2000);
        QTest::mouseClick(clearHits, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->item(1, 2)->text(), QStringLiteral("0"), 2000);
        shot(QStringLiteral("breakpoints"));
        /* the row's delete button */
        auto *rm = qobject_cast<QToolButton *>(t->cellWidget(1, 3));
        QVERIFY(rm);
        QTest::mouseClick(rm, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(studio2debug_breakpoint_list(m_d, list, 2), 1);
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        QCOMPARE(studio2debug_breakpoint_list(m_d, list, 2), 0);
    }

    QPlainTextEdit *memView()
    {
        return m_tabs->widget(2)->findChild<QPlainTextEdit *>();
    }

    /* Watches: add, live value, Memory, Break on change, Remove. */
    void watchesTab()
    {
        studio2watch_clear();
        studio2debug_write(m_d, 0x0870, 0x42);
        m_tabs->setCurrentIndex(4);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QTableWidget *t = m_tabs->currentWidget()->findChild<QTableWidget *>();
        QLineEdit *e = m_tabs->currentWidget()->findChild<QLineEdit *>();
        QVERIFY(t && e);
        QCOMPARE(t->rowCount(), 0);
        typeInto(e, QStringLiteral("$0870"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QVERIFY(t->item(0, 0)->text().contains(QStringLiteral("$0870")));
        QVERIFY(t->item(0, 0)->text().contains(QStringLiteral("$42")));
        studio2debug_write(m_d, 0x0870, 0x43);
        QTest::keyClick(m_win, Qt::Key_F7);             /* a stop refreshes the values */
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 0)->text().contains(QStringLiteral("$43")), 2000);
        typeInto(e, QStringLiteral("$0880-$088F"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        /* while the machine runs the value is a dash */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!studio2debug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 0)->text().contains(QLatin1String("-")) &&
                                     !t->item(0, 0)->text().contains(QStringLiteral("$43")), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("watches"));

        /* Break on change adds a write breakpoint with the condition */
        studio2debug_breakpoint_clear(m_d);
        auto buttonOf = [&](int row, const QString &text) -> QPushButton * {
            for (QPushButton *b : t->cellWidget(row, 1)->findChildren<QPushButton *>())
                if (b->text() == text) return b;
            return nullptr;
        };
        QPushButton *brk = buttonOf(0, QStringLiteral("Break on change"));
        QVERIFY(brk);
        QTest::mouseClick(brk, Qt::LeftButton);
        studio2debug_breakpoint bp[2];
        QCOMPARE(studio2debug_breakpoint_list(m_d, bp, 2), 1);
        QVERIFY(bp[0].type & STUDIO2DEBUG_BP_WRITE);
        QCOMPARE((int)bp[0].start, 0x0870);
        QVERIFY(QString::fromUtf8(bp[0].condition).contains(QStringLiteral("old")));
        studio2debug_breakpoint_clear(m_d);

        /* Memory switches to the Memory tab with the range marked */
        QPushButton *mem = buttonOf(1, QStringLiteral("Memory"));
        QVERIFY(mem);
        QTest::mouseClick(mem, Qt::LeftButton);
        QCOMPARE(m_tabs->currentIndex(), 2);
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$0880: ")));
        QVERIFY(!memView()->extraSelections().isEmpty());
        /* and every watch is marked in the dump, in its own colour */
        bool watched = false;
        for (const auto &s : memView()->extraSelections())
            watched |= s.format.background().color() == QColor(0x1c, 0x71, 0xd8);
        QVERIFY(watched);

        m_tabs->setCurrentIndex(4);
        QTest::mouseClick(buttonOf(1, QStringLiteral("Remove")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(studio2watch_count(), 1);
        studio2watch_clear();
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        m_tabs->setCurrentIndex(0);
    }

    /* Go to marks a range; Find Next / Previous step through hits. */
    void memorySearch()
    {
        const uint8_t pat[4] = { 0xDE, 0xEE, 0xDE, 0xEE };
        for (int i = 0; i < 4; ++i) studio2debug_write(m_d, (uint16_t)(0x0900 + i), pat[i]);
        for (int i = 0; i < 4; ++i) studio2debug_write(m_d, (uint16_t)(0x0980 + i), pat[i]);
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QLineEdit *go = entryIs(QStringLiteral("$0900, label or lo-hi"));
        typeInto(go, QStringLiteral("0900-0903"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$0900: DE EE DE EE")));
        QVERIFY(!memView()->extraSelections().isEmpty());
        /* a label also works */
        studio2debug_set_label(m_d, 0x0980, "found");
        typeInto(go, QStringLiteral("found"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$0980: DE EE")));
        studio2debug_set_label(m_d, 0x0980, nullptr);

        QLineEdit *find = entry(QStringLiteral("bytes or text"));
        QVERIFY(find);
        QLabel *note = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("gray")) && l->text().isEmpty()) note = l;
        QVERIFY(note);
        typeInto(go, QStringLiteral("0900-0903"));
        find->setText(QStringLiteral("DE EE DE"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QVERIFY2(note->text().startsWith(QLatin1String("Found at $")), qPrintable(note->text()));
        QCOMPARE(note->text(), QStringLiteral("Found at $0980"));          /* after the marked one */
        QTest::mouseClick(button(QStringLiteral("Find Previous")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $0900"));
        find->setText(QStringLiteral("DE EE DE EE FF 77 66"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Not found"));
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Not found"));
        /* text, case-insensitive */
        for (int i = 0; i < 5; ++i) studio2debug_write(m_d, (uint16_t)(0x09A0 + i), (uint8_t)"HeLLo"[i]);
        m_tabs->currentWidget()->findChild<QComboBox *>()->setCurrentText(QStringLiteral("Text"));
        find->setText(QStringLiteral("hello"));
        typeInto(go, QStringLiteral("0800"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $09A0"));
        shot(QStringLiteral("search"));
        m_tabs->currentWidget()->findChild<QComboBox *>()->setCurrentText(QStringLiteral("Hex"));
        /* it needs a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!studio2debug_is_stopped(m_d), 2000);
        find->setText(QStringLiteral("DE EE"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Stop the machine first"));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        m_tabs->setCurrentIndex(0);
    }

    /* Trace and History share the ring and the recorder. */
    void traceAndHistory()
    {
        studio2debug_trace_enable(m_d, 0);
        studio2debug_trace_clear(m_d);
        m_tabs->setCurrentIndex(6);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QPushButton *rec = m_tabs->currentWidget()->findChild<QPushButton *>();
        QVERIFY(rec && rec->text() == QLatin1String("Record"));
        QPushButton *rec2 = m_tabs->widget(7)->findChild<QPushButton *>();
        QVERIFY(rec2 && rec2 != rec);
        QVERIFY(!rec->isChecked());
        QTest::mouseClick(rec, Qt::LeftButton);
        QVERIFY(studio2debug_trace_enabled(m_d));
        QVERIFY(rec->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(rec2->isChecked(), 2000);                   /* in sync */
        QTest::keyClick(m_win, Qt::Key_F5);                                  /* runs */
        QTRY_VERIFY_WITH_TIMEOUT(!studio2debug_is_stopped(m_d), 2000);
        auto *tr = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(tr->toPlainText().contains(QStringLiteral("INC")), 3000);   /* live */
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QTRY_VERIFY_WITH_TIMEOUT(studio2debug_trace_count(m_d) > 20, 2000);
        /* History shows the same ring, oldest first, at the stop */
        m_tabs->setCurrentIndex(7);
        auto *hist = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(hist->toPlainText().contains(QStringLiteral("INC")), 2000);
        QVERIFY(hist->toPlainText().count(QLatin1Char('\n')) < 2000);
        /* newest at the bottom: the view is scrolled to its end */
        QVERIFY(hist->verticalScrollBar()->value() >= hist->verticalScrollBar()->maximum() - 1);
        QLabel *note = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().contains(QLatin1String("instructions"))) note = l;
        QVERIFY(note && note->text().startsWith(QLatin1String("recording")));
        shot(QStringLiteral("history"));
        /* a branches-only filter leaves branches, calls and returns */
        auto *filter = m_tabs->currentWidget()->findChild<QComboBox *>();
        filter->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!hist->toPlainText().contains(QStringLiteral("INC")), 2000);
        QVERIFY(hist->toPlainText().contains(QStringLiteral("SEP")) || hist->toPlainText().contains(QStringLiteral("BR")));
        /* a range filter */
        filter->setCurrentIndex(2);
        auto *range = m_tabs->currentWidget()->findChild<QLineEdit *>();
        typeInto(range, QStringLiteral("0830-0830"));
        QTRY_VERIFY_WITH_TIMEOUT(hist->toPlainText().contains(QStringLiteral("STR")) &&
                                     !hist->toPlainText().contains(QStringLiteral("INC")), 2000);
        filter->setCurrentIndex(0);
        /* Clear */
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear")) QTest::mouseClick(b, Qt::LeftButton);
        QCOMPARE(studio2debug_trace_count(m_d), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!hist->toPlainText().contains(QStringLiteral("INC")), 2000);
        /* and off, from either tab */
        QTest::mouseClick(m_tabs->widget(7)->findChild<QPushButton *>(), Qt::LeftButton);
        QVERIFY(!studio2debug_trace_enabled(m_d));
        QTRY_VERIFY_WITH_TIMEOUT(!m_tabs->widget(6)->findChild<QPushButton *>()->isChecked(), 2000);
        m_tabs->setCurrentIndex(0);
    }

    /* Any tab can leave for a window of its own and come back to its place. */
    void popOutAndDock()
    {
        m_tabs->setCurrentIndex(3);
        QPushButton *pop = button(QStringLiteral("Pop Out"));
        QVERIFY(pop);
        QTest::mouseClick(pop, Qt::LeftButton);
        QCOMPARE(m_tabs->count(), 11);
        QWidget *win = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("Studio II Debugger - Breakpoints")) win = w;
        QVERIFY(win);
        QTRY_VERIFY_WITH_TIMEOUT(win->isVisible(), 1000);
        QVERIFY(win->findChild<QTableWidget *>());
        QCOMPARE(win->property("studio2HelpContext").toString(), QStringLiteral("debugger/breakpoints"));
        /* the popped tab keeps refreshing: a breakpoint added shows in its window */
        studio2debug_breakpoint_toggle(m_d, kCall);
        QTRY_COMPARE_WITH_TIMEOUT(win->findChild<QTableWidget *>()->rowCount(), 1, 2000);
        studio2debug_breakpoint_clear(m_d);
        shot(QStringLiteral("popout"));
        QPushButton *dockBtn = nullptr;
        for (QPushButton *b : win->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Dock")) dockBtn = b;
        QVERIFY(dockBtn);
        QTest::mouseClick(dockBtn, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 12, 1000);
        QCOMPARE(m_tabs->tabText(3), QStringLiteral("Breakpoints"));       /* back in its place */
        /* closing the window docks it too; several can be out, but one stays */
        m_tabs->setCurrentIndex(5);
        QTest::mouseClick(pop, Qt::LeftButton);
        QCOMPARE(m_tabs->count(), 11);
        QCOMPARE(m_tabs->tabText(5), QStringLiteral("Trace"));
        QWidget *cart = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("Studio II Debugger - Cart & I/O")) cart = w;
        QVERIFY(cart);
        cart->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 12, 1000);
        QCOMPARE(m_tabs->tabText(5), QStringLiteral("Cart && I/O"));
        for (int i = 0; i < 11; ++i) {
            m_tabs->setCurrentIndex(0);
            QTest::mouseClick(pop, Qt::LeftButton);
        }
        QCOMPARE(m_tabs->count(), 1);
        QTest::mouseClick(pop, Qt::LeftButton);                          /* the last stays */
        QCOMPARE(m_tabs->count(), 1);
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle().startsWith(QLatin1String("Studio II Debugger - "))) w->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 12, 1000);
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names.first(), QStringLiteral("Console"));
        QCOMPARE(names.last(), QStringLiteral("FujiNet"));
        QCOMPARE(names.at(5), QStringLiteral("Cart && I/O"));
        QCOMPARE(names.at(10), QStringLiteral("Symbols"));
    }

    /* A new stop pulls every view back to the spot from wherever it was
     * scrolled: the disassembly to the PC, Memory to the PC, the Trace and
     * History to their newest line; and the Breakpoints tab says why. */
    void stopRecentresAndNames()
    {
        m_tabs->setCurrentIndex(1);
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x0823) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -600), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        /* Memory scrolled to the top, History away from its end */
        m_tabs->setCurrentIndex(2);
        memView()->verticalScrollBar()->setValue(0);
        m_tabs->setCurrentIndex(1);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_VERIFY_WITH_TIMEOUT(follow->isChecked(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, pcOf(m_d)) >= 0, 2000);
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(memView()->verticalScrollBar()->value() >= pcOf(m_d) / 16 - 80, 2000);
        m_tabs->setCurrentIndex(3);
        QLabel *stop = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped"))) stop = l;
        QVERIFY(stop);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(stop->text() == QLatin1String("Running."), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QVERIFY(button(QStringLiteral("Clear Hits")));
        m_tabs->setCurrentIndex(0);
    }

    /* Cart & I/O: the cartridge, the memory map once, the ports, the page
     * map, the text engine and the keypads; Sound; FujiNet. */
    void otherTabs()
    {
        m_tabs->setCurrentIndex(5);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        auto *io = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QVERIFY(io);
        QTRY_VERIFY_WITH_TIMEOUT(!io->toPlainText().isEmpty(), 2000);
        const QString text = io->toPlainText();
        QCOMPARE(text.count(QStringLiteral("Memory map")), 1);
        QVERIFY(text.contains(QStringLiteral("Key latch")));
        QVERIFY(text.contains(QStringLiteral("Keypad A")));
        QVERIFY(text.contains(QStringLiteral("BIOS")));
        QVERIFY(text.contains(QStringLiteral("$0800-$09FF")) || text.contains(QStringLiteral("RAM")));
        bool pads = false, engine = false;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>()) {
            pads |= l->text().contains(QLatin1String("Keypad B"));
            engine |= l->text() == QLatin1String("Text engine");
        }
        QVERIFY(pads && engine);
        shot(QStringLiteral("cart"));
        /* Sound: Q and the beeper's pitch */
        m_tabs->setCurrentIndex(9);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        auto *sound = qobject_cast<QPlainTextEdit *>(m_tabs->currentWidget());
        QVERIFY(sound);
        QTRY_VERIFY_WITH_TIMEOUT(sound->toPlainText().contains(QStringLiteral("Beeper (Q)")), 2000);
        QVERIFY(sound->toPlainText().contains(QStringLiteral("Pitch")));
        QVERIFY(!sound->toPlainText().contains(QStringLiteral("Memory map")));      /* in Cart & I/O */
        shot(QStringLiteral("sound"));
        /* FujiNet: empty without the runtime, and says so */
        m_tabs->setCurrentIndex(11);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        bool note = false;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            note |= l->text().contains(QLatin1String("FujiNet"));
        QVERIFY(note);
        QVERIFY(m_tabs->currentWidget()->findChild<QPushButton *>()->text() == QLatin1String("Clear"));
        shot(QStringLiteral("fujinet"));
        m_tabs->setCurrentIndex(0);
    }

    /* The Video tab: views, zoom, overlays, hover and pin, the tables and
     * Break on writes. */
    void videoTab()
    {
        studio2debug_breakpoint_clear(m_d);
        m_tabs->setCurrentIndex(8);
        QWidget *page = m_tabs->currentWidget();
        QTRY_VERIFY_WITH_TIMEOUT(page->isVisible(), 1000);
        auto combos = page->findChildren<QComboBox *>();
        QCOMPARE(combos.size(), 2);
        QComboBox *view = combos[0], *zoom = combos[1];
        QStringList views;
        for (int i = 0; i < view->count(); ++i) views << view->itemText(i);
        QCOMPARE(views, (QStringList{ QStringLiteral("Display"), QStringLiteral("Decoded"), QStringLiteral("Beam"),
                                      QStringLiteral("Values") }));
        QCOMPARE(zoom->itemText(0), QStringLiteral("Fit"));
        QWidget *pic = page->findChild<QWidget *>(QStringLiteral("videoPicture"));
        QVERIFY(pic);
        QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 64 && pic->height() >= 100, 2000);
        shot(QStringLiteral("video-display"));

        /* a change of view redraws, at the sizes the core names (integer zoom only) */
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("1×")));
        view->setCurrentIndex(0);
        QTRY_COMPARE_WITH_TIMEOUT(pic->width(), 100, 1000);
        QCOMPARE(pic->height(), 246);
        view->setCurrentIndex(1);
        QTRY_COMPARE_WITH_TIMEOUT(pic->width(), 64, 1000);
        QCOMPARE(pic->height(), 128);
        shot(QStringLiteral("video-decoded"));
        view->setCurrentIndex(2);
        QTRY_COMPARE_WITH_TIMEOUT(pic->width(), 112, 1000);
        QCOMPARE(pic->height(), 262);
        shot(QStringLiteral("video-beam"));
        view->setCurrentIndex(3);
        QTRY_COMPARE_WITH_TIMEOUT(pic->width(), 64, 1000);
        shot(QStringLiteral("video-values"));
        view->setCurrentIndex(1);
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("2×")));
        QTRY_COMPARE_WITH_TIMEOUT(pic->width(), 128, 1000);
        QCOMPARE(pic->height(), 256);
        view->setCurrentIndex(0);

        /* an overlay checkbox */
        QCheckBox *grid = nullptr;
        for (QCheckBox *box : page->findChildren<QCheckBox *>())
            if (box->text().contains(QLatin1String("rid")) || box->text().contains(QLatin1String("DMA byte"))) grid = box;
        QVERIFY(grid);
        grid->setChecked(true);
        QTest::qWait(50);
        shot(QStringLiteral("video-grid"));
        grid->setChecked(false);

        /* hover shows, a click pins, the same spot unpins */
        auto *tabs = page->findChild<QTabWidget *>();
        QVERIFY(tabs);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Pixel"));
        tabs->setCurrentIndex(0);
        QLabel *pixel = nullptr;
        for (QLabel *l : tabs->widget(0)->findChildren<QLabel *>()) pixel = l;
        QVERIFY(pixel);
        QVERIFY(pixel->text().contains(QLatin1String("Move over")));
        const QPoint at(pic->width() / 2, pic->height() / 2);
        QTest::mouseMove(pic, at);
        QTRY_VERIFY_WITH_TIMEOUT(!pixel->text().contains(QLatin1String("Move over")), 1000);
        QTest::mouseClick(pic, Qt::LeftButton, {}, at);
        QTest::mouseMove(pic, at + QPoint(30, 30));
        QVERIFY(!pixel->text().contains(QLatin1String("Move over")));   /* the pin stays */
        shot(QStringLiteral("video-pixel"));
        QTest::mouseClick(pic, Qt::LeftButton, {}, at);
        QTest::mouseMove(pic, QPoint(-5, -5));

        /* the tables */
        QTableWidget *regs = nullptr, *lines = nullptr, *colours = nullptr;
        for (QTableWidget *t : page->findChildren<QTableWidget *>()) {
            if (t->horizontalHeaderItem(0) && t->horizontalHeaderItem(0)->text() == QLatin1String("Name")) regs = t;
            if (t->horizontalHeaderItem(0) && t->horizontalHeaderItem(0)->text() == QLatin1String("Line")) lines = t;
            if (t->horizontalHeaderItem(1) && t->horizontalHeaderItem(1)->text() == QLatin1String("Entry")) colours = t;
        }
        QVERIFY(regs && lines && colours);
        tabs->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(regs->rowCount() > 0, 1000);
        shot(QStringLiteral("video-registers"));
        tabs->setCurrentIndex(2);
        QTRY_COMPARE_WITH_TIMEOUT(colours->rowCount(), 2, 1000);              /* two colours */
        shot(QStringLiteral("video-colours"));
        tabs->setCurrentIndex(3);
        QTRY_VERIFY_WITH_TIMEOUT(lines->rowCount() >= 128, 1000);
        shot(QStringLiteral("video-lines"));
        tabs->setCurrentIndex(4);
        QTest::qWait(50);
        shot(QStringLiteral("video-timing"));
        tabs->setCurrentIndex(0);

        /* Break on writes: with the display off nothing is behind the pixel, so the button is
         * off and says why; a program that points R0 at the BIOS display page and turns the
         * display on gives the 1861 console RAM to read, and then the pixel can be broken on */
        QPushButton *brk = nullptr;
        for (QPushButton *b : page->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Break on writes")) brk = b;
        QVERIFY(brk);
        for (size_t i = 0; i < sizeof kDisplayOn; i++) studio2debug_write(m_d, (uint16_t)(kDisplayAt + i), kDisplayOn[i]);
        for (size_t i = 0; i < sizeof kInterrupt; i++) studio2debug_write(m_d, (uint16_t)(kInterruptAt + i), kInterrupt[i]);
        studio2debug_cpu_set(m_d, STUDIO2_REG_PC, kDisplayAt);
        studio2debug_cpu_set(m_d, STUDIO2_REG_IE, 1);
        studio2debug_cpu c, c2;
        studio2debug_cpu_get(m_d, &c);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT((studio2debug_cpu_get(m_d, &c2), c2.frame >= c.frame + 3), 3000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        view->setCurrentIndex(1);
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("4×")));
        QTRY_COMPARE_WITH_TIMEOUT(pic->width(), 256, 1000);
        QTest::qWait(100);
        shot(QStringLiteral("video-decoded-on"));
        const QPoint top(40, 6);        /* the first DMA bytes of the display */
        QTest::mouseMove(pic, top);
        QTest::mouseClick(pic, Qt::LeftButton, {}, top);
        shot(QStringLiteral("video-pixel-on"));
        studio2debug_breakpoint bps[8];
        QTRY_VERIFY_WITH_TIMEOUT(brk->isEnabled(), 2000);
        QTest::mouseClick(brk, Qt::LeftButton);
        const int made = studio2debug_breakpoint_list(m_d, bps, 8);
        QCOMPARE(made, 1);
        QVERIFY(bps[0].type & STUDIO2DEBUG_BP_WRITE);
        QVERIFY2(bps[0].start >= 0x0800 && bps[0].start <= 0x09FF, qPrintable(QString::number(bps[0].start, 16)));
        studio2debug_breakpoint_clear(m_d);
        QTest::mouseClick(pic, Qt::LeftButton, {}, top);     /* unpin */
        QTest::mouseMove(pic, QPoint(-5, -5));
        QTRY_VERIFY_WITH_TIMEOUT(!brk->isEnabled(), 2000);
        zoom->setCurrentIndex(0);
        view->setCurrentIndex(0);
        m_tabs->setCurrentIndex(0);
    }

    /* The main window's F12 (display focused: its ShortcutOverride lets the
     * chord through to the View menu's action) and the View menu entry. */
    void f12FromTheMainWindow()
    {
        /* the display is a QRhiWidget; with no GPU device (CI's offscreen Qt)
         * the window swaps in the software display, so ask for whichever
         * is there */
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.vert.qsb")));
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.frag.qsb")));
        MainWindow main(m_s);
        main.show();
        QVERIFY(QTest::qWaitForWindowExposed(&main));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::qWait(300);   /* let a queued renderFailed swap the display */
        QWidget *display = main.centralWidget();
        QVERIFY(display);
        display->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::focusWidget() == display, 2000);
        QCOMPARE(QApplication::focusWidget(), display);
        QVERIFY(m_win->isVisible());
        QTest::keyClick(display, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!studio2debug_is_attached(m_d));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::keyClick(display, Qt::Key_F12);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(studio2debug_is_attached(m_d), 2000);
        waitStopped();
        /* the menu entry does the same */
        QAction *entry = nullptr;
        for (QAction *a : main.findChildren<QAction *>())
            if (a->text() == QLatin1String("&Debugger")) entry = a;
        QVERIFY(entry);
        QCOMPARE(entry->shortcut(), QKeySequence(Qt::Key_F12));
        entry->trigger();
        QVERIFY(!m_win->isVisible());
        entry->trigger();
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(studio2debug_is_attached(m_d), 2000);
        waitStopped();
    }

    /* A power cycle or media change keeps the engine attached: the new
     * machine stops at its first instruction and the window follows. */
    void sessionRestart()
    {
        m_tabs->setCurrentIndex(1);
        studio2debug_breakpoint_toggle(m_d, kCall);
        studio2session_stop(m_s);
        QCOMPARE(studio2session_start(m_s, &m_opts), 0);
        QTRY_VERIFY_WITH_TIMEOUT(studio2debug_is_stopped(m_d), 3000);
        QVERIFY(studio2debug_breakpoint_check(m_d, kCall));       /* kept */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(v->toPlainText().contains(QStringLiteral(">")), 2000);
        /* and, given the program again, it runs from there to the breakpoint */
        loadProgram(m_d);
        studio2debug_write(m_d, kCounter, 0);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(studio2debug_is_stopped(m_d) && pcOf(m_d) == kCall, 5000);
    }

    void f12HidesAndDetaches()
    {
        QVERIFY(m_win->isVisible());
        QTest::keyClick(m_win, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!studio2debug_is_attached(m_d));
        studio2debug_cpu c, c2;
        studio2debug_cpu_get(m_d, &c);
        QTRY_VERIFY_WITH_TIMEOUT((studio2debug_cpu_get(m_d, &c2), c2.frame > c.frame + 3), 3000);
        /* the main window's F12 is toggleFor: it shows the window and attaches (stopping) */
        DebuggerWindow::toggleFor(nullptr, m_s);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(studio2debug_is_attached(m_d), 2000);
        waitStopped();
        DebuggerWindow::toggleFor(nullptr, m_s);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!studio2debug_is_attached(m_d));
        DebuggerWindow::toggleFor(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(studio2debug_is_attached(m_d), 2000);
        waitStopped();
    }
};

QTEST_MAIN(DebuggerTest)
#include "debugger_test.moc"
