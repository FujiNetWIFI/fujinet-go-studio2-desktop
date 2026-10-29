/*
 * MainWindow -- see MainWindow.h.
 *
 * Plain Qt6 Widgets, deliberately not KDE Frameworks: it picks up Breeze
 * through the platform theme anyway, and staying framework-free keeps this
 * frontend usable outside a KDE session.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MainWindow.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFileDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QStatusBar>
#include <QUrl>

#include "DisplayWidget.h"
#include "FujiNetWindows.h"
#include "KeyForward.h"
#include "SettingsDialog.h"
#include "debugger/DebuggerWindow.h"
#include "keypad/KeypadWindow.h"

static const char *const kCartFilter =
    "RCA Studio II cartridges (*.st2 *.bin *.rom *.ST2 *.BIN *.ROM);;"
    "All files (*)";

MainWindow::MainWindow(studio2session *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go Studio II"));
    /* the 246 lines three times over, at the TV's 4:3 */
    resize(984, 738 + 60);
    setAcceptDrops(true);

    m_display = new DisplayWidget(session, this);
    setCentralWidget(m_display);

    m_dot = new QLabel(QStringLiteral("●"));
    m_status = new QLabel(QStringLiteral("Starting..."));
    statusBar()->addWidget(m_dot);
    statusBar()->addWidget(m_status);

    buildMenus();
    applyPicture();

    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatus);
    m_statusTimer.start(1000);
    updateStatus();
    /* The gamepad thread cannot call into Qt; it posts system actions and
     * this timer takes them. */
    connect(&m_sysactTimer, &QTimer::timeout, this, &MainWindow::drainSysactions);
    m_sysactTimer.start(100);
    /* Gamepads come and go on their own thread; say so when they do. */
    m_padGen = studio2session_gamepad_generation(session);
    connect(&m_padTimer, &QTimer::timeout, this, &MainWindow::pollGamepads);
    m_padTimer.start(250);

    if (qEnvironmentVariableIsSet("STUDIO2_OPEN_KEYPAD"))
        toggleKeypad();
    /* once the event loop runs, i.e. after main() has started the session:
     * attaching before the machine exists would not stop it */
    if (qEnvironmentVariableIsSet("STUDIO2_OPEN_DEBUGGER"))
        QTimer::singleShot(0, this, [this] { DebuggerWindow::showFor(this, m_session); });
    if (qEnvironmentVariableIsSet("STUDIO2_OPEN_SETTINGS"))
        QTimer::singleShot(0, this, &MainWindow::showSettings);
}

void MainWindow::buildMenus()
{
    QMenu *machine = menuBar()->addMenu(QStringLiteral("&Machine"));
    machine->addAction(QStringLiteral("&Open Cartridge..."), QKeySequence(Qt::CTRL | Qt::Key_O), this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open Cartridge"), QString(), QString::fromUtf8(kCartFilter));
        if (!f.isEmpty()) openCart(f);
    });
    machine->addAction(QStringLiteral("&Eject Cartridge"), this, [this] {
        studio2session_eject(m_session);
        statusBar()->showMessage(QStringLiteral("Cartridge ejected — back to CONFIG"), 4000);
    });
    machine->addAction(QStringLiteral("&Import Cartridge to SD..."), this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, QStringLiteral("Import Cartridge to SD"), QString(), QString::fromUtf8(kCartFilter));
        if (f.isEmpty()) return;
        char dest[1024];
        if (studio2session_import_cart_to_sd(m_session, f.toLocal8Bit().constData(), dest, sizeof dest) != 0)
            QMessageBox::warning(this, QStringLiteral("Import failed"),
                                 QString::fromUtf8(studio2session_last_error(m_session)));
        else
            statusBar()->showMessage(QStringLiteral("%1 is on the SD host — boot it from the CONFIG client")
                                     .arg(QFileInfo(QString::fromUtf8(dest)).fileName()), 6000);
    });
    machine->addAction(QStringLiteral("Import &BIOS..."), this,
                       [this] { SettingsDialog::importBios(this, m_session); });
    machine->addSeparator();
    /* CLEAR and Reset to CONFIG act through their key bindings (F3 and
     * Escape by default, remappable); the menu shows the bound key, it does
     * not claim it as a shortcut, so the binding stays the one that acts. */
    m_clearAction = machine->addAction(QStringLiteral("C&LEAR"), this,
                                       [this] { studio2session_clear(m_session); });
    m_resetConfigAction = machine->addAction(QStringLiteral("Reset to &CONFIG"), this,
                                             [this] { runSysaction(STUDIO2_SYSACT_RESET_CONFIG); });
    connect(machine, &QMenu::aboutToShow, this, &MainWindow::syncMachineMenu);
    syncMachineMenu();
    machine->addSeparator();
    machine->addAction(QStringLiteral("P&references..."), QKeySequence(Qt::CTRL | Qt::Key_Comma), this,
                       &MainWindow::showSettings);
    machine->addSeparator();
    machine->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, this, [this] { close(); });

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(QStringLiteral("&Keypad"), QKeySequence(Qt::Key_F9), this,
                    &MainWindow::toggleKeypad);
    view->addAction(QStringLiteral("&Debugger"), QKeySequence(Qt::Key_F12), this,
                    [this] { DebuggerWindow::toggleFor(this, m_session); });
    view->addSeparator();
    m_tvAction = view->addAction(QStringLiteral("&TV Aspect"));
    m_tvAction->setCheckable(true);
    m_tvAction->setToolTip(QStringLiteral("Show the picture across a 4:3 television, as the console "
                                          "does; off shows square pixels (a tall, narrow picture)"));
    connect(m_tvAction, &QAction::triggered, this, [this](bool on) {
        /* the family's "aspect": 0 the TV's 4:3, 1 square pixels */
        studio2session_set_int(m_session, "aspect", on ? 0 : 1);
        applyPicture();
    });
    m_smoothAction = view->addAction(QStringLiteral("&Smooth Scaling"));
    m_smoothAction->setCheckable(true);
    connect(m_smoothAction, &QAction::triggered, this, [this](bool on) {
        studio2session_set_int(m_session, "smooth", on ? 1 : 0);
        applyPicture();
    });
    view->addAction(QStringLiteral("&Fullscreen"), QKeySequence(Qt::Key_F11), this, [this] {
        if (isFullScreen()) showNormal(); else showFullScreen();
    });

    QMenu *fuji = menuBar()->addMenu(QStringLiteral("&FujiNet"));
    fuji->addAction(QStringLiteral("&Web UI"), this, [this] {
        if (studio2session_fujinet_external(m_session)) {
            statusBar()->showMessage(QStringLiteral("FujiNet is external: use its own web UI"), 4000);
            return;
        }
        if (!studio2session_fujinet_running(m_session)) {
            statusBar()->showMessage(QStringLiteral("FujiNet is not running"), 4000);
            return;
        }
        fujinet_config_show(this, m_session);
    });
    fuji->addAction(QStringLiteral("Console &Log"), this, [this] {
        if (studio2session_fujinet_external(m_session)) {
            statusBar()->showMessage(QStringLiteral("FujiNet is external: see its own console"), 4000);
            return;
        }
        fujinet_log_show(this, m_session);
    });

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(QStringLiteral("&About FujiNet Go Studio II"), this, &MainWindow::showAbout);
}

/* The bound key of each console control as the menu's shortcut text (it
 * follows a rebinding). */
void MainWindow::syncMachineMenu()
{
    auto label = [this](QAction *a, const QString &text, int target) {
        const studio2_binding b = studio2session_binding_get(m_session, target);
        char key[32] = "";
        if (b.keysym) studio2session_keysym_name(b.keysym, key, sizeof key);
        a->setText(key[0] ? text + QLatin1Char('\t') + QString::fromUtf8(key) : text);
    };
    label(m_clearAction, QStringLiteral("C&LEAR"), STUDIO2_TARGET_SWITCH(STUDIO2_SW_CLEAR));
    label(m_resetConfigAction, QStringLiteral("Reset to &CONFIG"), STUDIO2_TARGET_SYSACT(STUDIO2_SYSACT_RESET_CONFIG));
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, QStringLiteral("About FujiNet Go Studio II"),
        QStringLiteral("<b>FujiNet Go Studio II</b> %1<br><br>"
                       "RCA Studio II with built-in FujiNet.<br><br>"
                       "The emulator is MAME's RCA Studio II, CDP1802 and CDP1861 "
                       "(Curt Coder, BSD-3-Clause), transposed to C. The FujiNet firmware "
                       "and the cartridge are by Thomas Cherryhomes and the FujiNet "
                       "contributors (GPL-3.0-or-later).<br><br>"
                       "Copyright © 2026 Thomas Cherryhomes — GPL-3.0-or-later<br>"
                       "<a href=\"https://fujinet.online/\">fujinet.online</a> · "
                       "<a href=\"https://github.com/FujiNetWIFI/fujinet-go-studio2-desktop/issues\">"
                       "Report an issue</a>")
            .arg(QStringLiteral(STUDIO2_VERSION_STRING)));
}

void MainWindow::showSettings()
{
    const bool restart = SettingsDialog::run(this, m_session);
    applyPicture();
    syncMachineMenu();
    if (restart) restartSession();
}

void MainWindow::applyPicture()
{
    const bool tv = studio2session_get_int(m_session, "aspect", 0) == 0;
    const bool smooth = studio2session_get_int(m_session, "smooth", 0) != 0;
    m_display->setTvAspect(tv);
    m_display->setSmooth(smooth);
    if (m_tvAction) m_tvAction->setChecked(tv);
    if (m_smoothAction) m_smoothAction->setChecked(smooth);
}

void MainWindow::pollGamepads()
{
    const unsigned gen = studio2session_gamepad_generation(m_session);
    if (gen == m_padGen) return;
    m_padGen = gen;
    char msg[160];
    if (studio2session_gamepad_last_event(m_session, msg, sizeof msg) > 0)
        statusBar()->showMessage(QString::fromUtf8(msg), 4000);
}

/* A machine option changed: a power cycle with the new settings (a full
 * restart when FujiNet, audio or the gamepads were switched). */
void MainWindow::restartSession()
{
    if (studio2session_restart(m_session) != 0) {
        QMessageBox::warning(this, QStringLiteral("Restart failed"),
                             QString::fromUtf8(studio2session_last_error(m_session)));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Machine options applied (power cycled)"), 5000);
}

void MainWindow::toggleKeypad()
{
    if (!m_keypad) m_keypad = new KeypadWindow(m_session, this);
    if (m_keypad->isVisible()) m_keypad->hide();
    else m_keypad->show();
}

void MainWindow::runSysaction(int sa)
{
    switch (sa) {
    case STUDIO2_SYSACT_RESET_CONFIG:
        studio2session_sysaction(m_session, sa);
        statusBar()->showMessage(QStringLiteral("Back to the FujiNet CONFIG client"), 4000);
        break;
    case STUDIO2_SYSACT_DEBUG_STOP:
        DebuggerWindow::showFor(this, m_session);
        break;
    default: break;
    }
}

void MainWindow::drainSysactions()
{
    int sa;
    while (studio2session_sysaction_take(m_session, &sa)) runSysaction(sa);
}

void MainWindow::updateStatus()
{
    QString text;
    bool on = false;
    const QString cart = QString::fromUtf8(studio2session_cart_path(m_session));
    if (!studio2session_is_running(m_session)) {
        text = QStringLiteral("Stopped");
    } else {
        char st[160];
        studio2session_cart_status(m_session, st, sizeof st);
        on = studio2session_cart_link_up(m_session) == 1;
        text = QStringLiteral("FujiNet: %1").arg(QString::fromUtf8(st));
        if (!cart.isEmpty())
            text = QStringLiteral("%1 — %2").arg(QFileInfo(cart).fileName(), text);
        else if (!studio2session_cart_booted_game(m_session))
            text = QStringLiteral("CONFIG — %1").arg(text);
    }
    m_status->setText(text);
    m_dot->setStyleSheet(on ? QStringLiteral("color: %1;").arg(studio2AccentTextColor(this).name())
                            : QStringLiteral("color: gray;"));
    checkNeedsBios();
}

/* CONFIG booted a game from the network with no BIOS to run it: say so
 * once, with the way out. Shown again only after the flag has cleared (a
 * power cycle) and come back. */
void MainWindow::checkNeedsBios()
{
    if (!studio2session_needs_bios(m_session)) {
        m_needsBiosShown = false;
        return;
    }
    if (m_needsBiosShown) return;
    m_needsBiosShown = true;
    /* not exec(): this runs from a timer, and a nested event loop would
     * run it again */
    auto *box = new QMessageBox(QMessageBox::Information, QStringLiteral("BIOS needed"),
                                QStringLiteral("This cartridge needs the RCA Studio II BIOS"),
                                QMessageBox::NoButton, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setInformativeText(QStringLiteral("Games are written in the BIOS's byte-code, so only the "
                                           "real BIOS runs them; CONFIG and the FujiNet apps run "
                                           "without it. Import your own copy and the console goes "
                                           "back to CONFIG to boot the game again."));
    QPushButton *import = box->addButton(QStringLiteral("Import BIOS..."), QMessageBox::AcceptRole);
    box->addButton(QStringLiteral("Not Now"), QMessageBox::RejectRole);
    connect(box, &QMessageBox::buttonClicked, this, [this, import](QAbstractButton *b) {
        if (b != import) return;
        if (SettingsDialog::importBios(this, m_session) < 0) return;
        runSysaction(STUDIO2_SYSACT_RESET_CONFIG);
    });
    box->open();
}

void MainWindow::openCart(const QString &path)
{
    if (studio2session_load_cart(m_session, path.toLocal8Bit().constData()) != 0) {
        QMessageBox::warning(this, QStringLiteral("Could not open"),
                             QString::fromUtf8(studio2session_last_error(m_session)));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Running %1").arg(QFileInfo(path).fileName()), 4000);
}

void MainWindow::loadMedia(const QString &path)
{
    const QByteArray p = path.toLocal8Bit();
    /* a BIOS first: one may well be named .bin, as a cartridge can be */
    if (studio2session_media_is_bios(p.constData())) {
        SettingsDialog::importBios(this, m_session, path);
        return;
    }
    if (studio2session_media_is_cartridge(p.constData())) {
        openCart(path);
        return;
    }
    char dest[1024];
    if (studio2session_import_media(m_session, p.constData(), dest, sizeof dest) != 0) {
        QMessageBox::warning(this, QStringLiteral("Import failed"),
                             QString::fromUtf8(studio2session_last_error(m_session)));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Copied to FujiNet's SD folder — mount it from the CONFIG client"), 6000);
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    if (e->key() == Qt::Key_F9) { toggleKeypad(); return; }
    if (e->key() == Qt::Key_F12) { DebuggerWindow::toggleFor(this, m_session); return; }
    /* Ctrl chords are the menus' shortcuts. Alt is not: Left Alt is keypad
     * A's 0 (MAME's key), and the menus' Alt+letter mnemonics are taken as
     * shortcuts before a key event ever gets here. */
    if (e->modifiers() & Qt::ControlModifier) { QMainWindow::keyPressEvent(e); return; }

    const uint32_t ks = studio2KeysymFromQt(e);
    if (!ks) { QMainWindow::keyPressEvent(e); return; }

    const int sa = studio2session_key_sysaction(m_session, ks);
    if (sa >= 0) {
        if (!m_sysactDown[sa]) { m_sysactDown[sa] = true; runSysaction(sa); }
        return;
    }
    if (!studio2session_key(m_session, ks, 1)) QMainWindow::keyPressEvent(e);
}

void MainWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    const uint32_t ks = studio2KeysymFromQt(e);
    if (!ks) { QMainWindow::keyReleaseEvent(e); return; }
    const int sa = studio2session_key_sysaction(m_session, ks);
    if (sa >= 0) { m_sysactDown[sa] = false; return; }
    if (!studio2session_key(m_session, ks, 0)) QMainWindow::keyReleaseEvent(e);
}

bool MainWindow::event(QEvent *e)
{
    if (e->type() == QEvent::WindowDeactivate) {
        studio2session_release_all(m_session);
        for (bool &d : m_sysactDown) d = false;
    }
    return QMainWindow::event(e);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    if (urls.isEmpty()) return;
    const QString path = urls.first().toLocalFile();
    if (!path.isEmpty()) loadMedia(path);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    m_statusTimer.stop();
    m_sysactTimer.stop();
    m_padTimer.stop();
    QMainWindow::closeEvent(e);
}
