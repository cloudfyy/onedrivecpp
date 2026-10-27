#include "main_window.hpp"
#include "autostart.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>

#include <exception>

namespace onedrive::gui {

MainWindow::~MainWindow() {
    QApplication::setQuitOnLastWindowClosed(quit_on_last_window_closed_);
}

void MainWindow::create_system_tray() {
    quit_on_last_window_closed_ = QApplication::quitOnLastWindowClosed();
    tray_ = new QSystemTrayIcon{windowIcon(), this};
    tray_->setObjectName("systemTray");
    auto* menu = new QMenu{this};
    tray_open_window_ = menu->addAction(QString{});
    tray_open_folder_ = menu->addAction(QString{});
    tray_autostart_ = menu->addAction(QString{});
    tray_autostart_->setObjectName("startAtLoginAction");
    tray_autostart_->setCheckable(true);
    connect(
        tray_autostart_,
        &QAction::triggered,
        this,
        &MainWindow::set_start_at_login
    );
    menu->addSeparator();
    tray_quit_ = menu->addAction(QString{});
    tray_->setContextMenu(menu);
    connect(
        tray_open_window_,
        &QAction::triggered,
        this,
        &MainWindow::show_main_window
    );
    connect(
        tray_open_folder_,
        &QAction::triggered,
        this,
        &MainWindow::open_sync_folder
    );
    connect(tray_quit_, &QAction::triggered, this, &MainWindow::request_quit);
    connect(
        tray_,
        &QSystemTrayIcon::activated,
        this,
        [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger ||
                reason == QSystemTrayIcon::DoubleClick) {
                show_main_window();
            }
        }
    );
    connect(menu, &QMenu::aboutToShow, this, [this] {
        refresh_autostart();
        tray_open_folder_->setEnabled(!state_.sync_directory.empty());
    });
    // Qt registers the icon automatically if a tray becomes available later.
    tray_->show();
    tray_timer_ = new QTimer{this};
    tray_timer_->setObjectName("trayAvailabilityTimer");
    tray_timer_->setInterval(2000);
    connect(
        tray_timer_,
        &QTimer::timeout,
        this,
        &MainWindow::refresh_tray_availability
    );
    tray_timer_->start();
    refresh_tray_availability();
}

void MainWindow::update_tray_tooltip() {
    if (!tray_) {
        return;
    }
    QStringList lines{tr("onedrive client")};
    if (sync_status_ && !sync_status_->text().isEmpty()) {
        lines << sync_status_->text();
    }
    if (!state_.sync_directory.empty()) {
        lines << tr("Sync directory: %1")
                     .arg(
                         QString::fromStdString(state_.sync_directory.string())
                     );
    }
    tray_->setToolTip(lines.join('\n'));
}

void MainWindow::show_at_startup(bool start_in_tray) {
    refresh_tray_availability();
    if (!start_in_tray || !tray_available_) {
        show_main_window();
    }
}

void MainWindow::refresh_autostart() {
    const QSignalBlocker checkbox_blocker{autostart_checkbox_};
    const QSignalBlocker action_blocker{tray_autostart_};
    try {
        const bool enabled = autostart_enabled();
        autostart_checkbox_->setChecked(enabled);
        tray_autostart_->setChecked(enabled);
        autostart_checkbox_->setEnabled(true);
        tray_autostart_->setEnabled(true);
    } catch (const std::exception& error) {
        autostart_checkbox_->setEnabled(false);
        tray_autostart_->setEnabled(false);
        log_message(
            LogOperation::configuration,
            events::MessageKind::error,
            QT_TR_NOOP("Cannot read automatic startup settings:\n%1"),
            QString::fromUtf8(error.what())
        );
    }
}

void MainWindow::set_start_at_login(bool enabled) {
    try {
        set_autostart_enabled(enabled);
        refresh_autostart();
    } catch (const std::exception& error) {
        refresh_autostart();
        const auto detail = QString::fromUtf8(error.what());
        log_message(
            LogOperation::configuration,
            events::MessageKind::error,
            QT_TR_NOOP("Cannot change automatic startup settings:\n%1"),
            detail
        );
        show_main_window();
        QMessageBox::warning(
            this,
            tr("Cannot change automatic startup"),
            tr("Cannot change automatic startup settings:\n%1").arg(detail)
        );
    }
}

void MainWindow::refresh_tray_availability() {
    const bool available = QSystemTrayIcon::isSystemTrayAvailable() &&
                           tray_->isVisible() && !tray_->icon().isNull();
    if (tray_available_ && !available && !isVisible() && !closing_) {
        show_main_window();
    }
    tray_available_ = available;
    QApplication::setQuitOnLastWindowClosed(
        available ? false : quit_on_last_window_closed_
    );
}

void MainWindow::show_main_window() {
    if (isMinimized()) {
        setWindowState(windowState() & ~Qt::WindowMinimized);
    }
    show();
    raise();
    activateWindow();
}

void MainWindow::open_sync_folder() {
    if (state_.sync_directory.empty()) {
        return;
    }
    const auto directory =
        QString::fromStdString(state_.sync_directory.string());
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(directory))) {
        show_main_window();
        QMessageBox::warning(
            this,
            tr("Cannot open sync folder"),
            tr("Cannot open the sync folder:\n%1").arg(directory)
        );
    }
}

void MainWindow::request_quit() {
    show_main_window();
    quit_requested_ = true;
    close();
}

} // namespace onedrive::gui
