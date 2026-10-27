#include "synchronization_support.hpp"
#include "synchronization_cases.hpp"
#include "autostart.hpp"

#include <QAction>
#include <QAbstractButton>
#include <QMenu>
#include <QMessageBox>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QProcess>
#include <QProcessEnvironment>

namespace {

bool tray_available{};
QUrl opened_folder;
bool open_folder_succeeds{true};

struct TrayEnvironment {
    bool previous_quit{QApplication::quitOnLastWindowClosed()};
    TrayEnvironment() {
        tray_available = true;
        QApplication::setQuitOnLastWindowClosed(true);
    }
    ~TrayEnvironment() {
        tray_available = false;
        QApplication::setQuitOnLastWindowClosed(previous_quit);
    }
};

struct ConfigEnvironment {
    QByteArray previous{qgetenv("XDG_CONFIG_HOME")};
    bool present{qEnvironmentVariableIsSet("XDG_CONFIG_HOME")};
    explicit ConfigEnvironment(const QString& path) {
        qputenv("XDG_CONFIG_HOME", path.toUtf8());
    }
    ~ConfigEnvironment() {
        if (present) {
            qputenv("XDG_CONFIG_HOME", previous);
        } else {
            qunsetenv("XDG_CONFIG_HOME");
        }
    }
};

} // namespace

extern "C" bool __wrap__ZN15QSystemTrayIcon21isSystemTrayAvailableEv() {
    return tray_available;
}

extern "C" bool __wrap__ZN16QDesktopServices7openUrlERK4QUrl(const QUrl& url) {
    opened_folder = url;
    return open_folder_succeeds;
}

namespace onedrive::test::gui_sync {
namespace {

QSystemTrayIcon* tray(gui::MainWindow& window) {
    auto* icon = window.findChild<QSystemTrayIcon*>("systemTray");
    require(icon && icon->contextMenu(), "missing system tray or context menu");
    return icon;
}

QAction* tray_action(gui::MainWindow& window, const QString& text) {
    for (auto* action : tray(window)->contextMenu()->actions()) {
        if (action->text() == text) {
            return action;
        }
    }
    throw std::runtime_error{"missing tray action"};
}

void refresh_tray(gui::MainWindow& window) {
    auto* timer = window.findChild<QTimer*>("trayAvailabilityTimer");
    require(
        timer &&
            QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection),
        "tray availability check did not run"
    );
}

} // namespace

void test_tray_window_lifecycle() {
    TrayEnvironment environment;
    tray_available = false;
    Fixture fixture;
    auto state = gui::load_app_state_view_model(fixture.file);
    state.sync_directory = fixture.root.path() / "同步 space %1";
    gui::MainWindow window{state};
    window.show();
    require(
        QApplication::quitOnLastWindowClosed(),
        "unavailable tray disabled normal last-window shutdown"
    );
    tray_available = true;
    refresh_tray(window);
    require(
        !QApplication::quitOnLastWindowClosed() && tray(window)->isVisible() &&
            tray(window)->icon().cacheKey() == window.windowIcon().cacheKey(),
        "late tray availability did not keep the application accessible"
    );
    window.close();
    require(!window.isVisible(), "closing a tray window did not hide it");
    tray(window)->activated(QSystemTrayIcon::MiddleClick);
    tray(window)->activated(QSystemTrayIcon::Context);
    require(
        !window.isVisible(), "middle/right click performed a primary action"
    );
    tray(window)->activated(QSystemTrayIcon::Trigger);
    require(window.isVisible(), "left click did not open the window");
    tray(window)->activated(QSystemTrayIcon::Trigger);
    require(window.isVisible(), "left click toggled the window closed");
    window.showMinimized();
    tray(window)->activated(QSystemTrayIcon::Trigger);
    require(
        window.isVisible() && !window.isMinimized(),
        "left click did not restore a minimized window"
    );
    window.close();
    tray_action(window, "Open window")->trigger();
    require(window.isVisible(), "context menu could not reopen the window");
    tray_action(window, "Open sync folder")->trigger();
    require(
        opened_folder.isLocalFile() &&
            opened_folder.toLocalFile() ==
                QString::fromStdString(state.sync_directory.string()),
        "sync folder action did not use a correctly encoded local URL"
    );
    window.close();
    tray_available = false;
    refresh_tray(window);
    require(
        window.isVisible() && QApplication::quitOnLastWindowClosed(),
        "disappearing tray stranded the hidden application"
    );
    require(window.close(), "unavailable tray still intercepted window close");
}

void test_tray_startup_visibility() {
    TrayEnvironment environment;
    Fixture fixture;
    {
        gui::MainWindow window{gui::load_app_state_view_model(fixture.file)};
        window.show_at_startup(true);
        require(
            !window.isVisible() && tray(window)->isVisible() &&
                !QApplication::quitOnLastWindowClosed(),
            "tray startup showed a window or allowed background shutdown"
        );
        tray_available = false;
        refresh_tray(window);
        require(window.isVisible(), "tray loss stranded an autostarted client");
    }
    {
        gui::MainWindow window{gui::load_app_state_view_model(fixture.file)};
        window.show_at_startup(true);
        require(
            window.isVisible(), "tray startup without a tray hid the window"
        );
    }
    tray_available = true;
    gui::MainWindow window{gui::load_app_state_view_model(fixture.file)};
    window.show_at_startup(false);
    require(window.isVisible(), "normal startup unexpectedly hid the window");
}

void test_autostart_settings() {
    TrayEnvironment environment;
    QTemporaryDir directory;
    require(directory.isValid(), "cannot create isolated autostart directory");
    ConfigEnvironment config_environment{directory.path()};
    require(
        gui::autostart_file_path() ==
            directory.filePath("autostart/onedrive-cpp-gui.desktop"),
        "autostart ignored the user XDG configuration directory"
    );
    Fixture fixture;
    gui::MainWindow window{gui::load_app_state_view_model(fixture.file)};
    auto* checkbox = window.findChild<QCheckBox*>("startAtLogin");
    auto* action = window.findChild<QAction*>("startAtLoginAction");
    require(
        checkbox && action && action->isCheckable() && !checkbox->isChecked() &&
            !action->isChecked(),
        "autostart controls did not default to disabled"
    );
    checkbox->setChecked(true);
    require(
        gui::autostart_enabled() && action->isChecked() &&
            !window.windowTitle().endsWith(" *"),
        "settings did not immediately enable and synchronize autostart"
    );
    {
        QSettings entry{gui::autostart_file_path(), QSettings::IniFormat};
        const auto contents = QString::fromStdString(
            test::read_file(gui::autostart_file_path().toStdString())
        );
        require(
            contents.contains(
                "Exec=\"" + QCoreApplication::applicationFilePath() +
                "\" --start-in-tray\n"
            ) && !entry.value("Desktop Entry/Terminal").toBool(),
            "autostart did not persist the GUI executable and tray option"
        );
        gui::MainWindow reopened{gui::load_app_state_view_model(fixture.file)};
        require(
            reopened.findChild<QCheckBox*>("startAtLogin")->isChecked(),
            "autostart setting was not restored in a new window"
        );
    }
    action->trigger();
    require(
        !gui::autostart_enabled() && !checkbox->isChecked() &&
            !action->isChecked(),
        "tray menu did not disable and synchronize autostart"
    );
    auto* draft = window.findChild<QLineEdit*>("auth.application_id");
    draft->setText("unsaved-client");
    action->trigger();
    require(
        gui::autostart_enabled() && checkbox->isChecked() &&
            draft->text() == "unsaved-client" &&
            window.windowTitle().endsWith(" *"),
        "tray enabling autostart discarded the TOML draft or its dirty "
        "indicator"
    );
    checkbox->setChecked(false);
    require(
        !gui::autostart_enabled() && !action->isChecked() &&
            draft->text() == "unsaved-client" &&
            config::Config::load(fixture.file).application_id == "test-client",
        "settings disabling autostart changed or saved the TOML draft"
    );
    gui::set_autostart_enabled(true);
    QMetaObject::invokeMethod(
        tray(window)->contextMenu(), "aboutToShow", Qt::DirectConnection
    );
    require(
        checkbox->isChecked() && action->isChecked(),
        "menu did not refresh externally changed autostart settings"
    );
    gui::set_autostart_enabled(false);
    QMetaObject::invokeMethod(
        tray(window)->contextMenu(), "aboutToShow", Qt::DirectConnection
    );
    require(
        QFile::remove(gui::autostart_file_path()) &&
            QDir{directory.path()}.rmdir("autostart"),
        "could not prepare autostart write failure"
    );
    QFile obstruction{directory.filePath("autostart")};
    require(
        obstruction.open(QIODevice::WriteOnly), "cannot obstruct autostart"
    );
    obstruction.close();
    bool warned = false;
    QTimer::singleShot(0, &window, [&] {
        if (auto* dialog =
                qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            warned = dialog->text().contains("Cannot change automatic startup");
            dialog->accept();
        }
    });
    checkbox->setChecked(true);
    require(
        warned && !checkbox->isChecked() && !action->isChecked() &&
            !gui::autostart_enabled(),
        "autostart write failure was hidden or left controls enabled"
    );
}

void test_autostart_entry_failures() {
    TrayEnvironment environment;
    QTemporaryDir directory;
    require(directory.isValid(), "cannot create isolated autostart directory");
    ConfigEnvironment config_environment{directory.path()};
    Fixture fixture;
    gui::set_autostart_enabled(true);
    const auto path =
        std::filesystem::path{gui::autostart_file_path().toStdString()};
    test::write_file(
        path,
        "# User desktop startup\n[Desktop Entry]\n"
        "Type = Application\nExec = \"client with spaces\" --start-in-tray\n\n"
        "[Desktop Action Other]\nHidden=true\n"
    );
    require(
        gui::autostart_enabled(),
        "comments, whitespace or another section changed default startup "
        "visibility"
    );
    test::write_file(
        path,
        "[Desktop Entry]\nType=Application\nExec=client\nHidden=true\n"
        "[Desktop Action Other]\nHidden=false\n"
    );
    require(
        !gui::autostart_enabled(),
        "another section overrode the disabled startup setting"
    );
    for (const auto* contents :
         {"[Desktop Entry]\nType=Link\nExec=wrong-type\n",
          "[Desktop Entry]\nType=Application\n",
          "[Desktop Entry]\nType=Application\nExec=client\nHidden=invalid\n",
          "[Desktop Entry\nType=Application\nExec=broken-section\n"}) {
        test::write_file(path, contents);
        require(
            test::throws_with<std::runtime_error>(
                [] { static_cast<void>(gui::autostart_enabled()); },
                "Cannot read the autostart entry"
            ),
            "malformed autostart entry was silently treated as a valid setting"
        );
        gui::MainWindow window{gui::load_app_state_view_model(fixture.file)};
        auto* checkbox = window.findChild<QCheckBox*>("startAtLogin");
        auto* action = window.findChild<QAction*>("startAtLoginAction");
        const auto logs =
            window.findChild<QPlainTextEdit*>("logMessages")->toPlainText();
        if (checkbox->isEnabled() || action->isEnabled() ||
            !logs.contains("Cannot read automatic startup settings:") ||
            !logs.contains(gui::autostart_file_path())) {
            throw std::runtime_error{
                std::string{
                    "Unreadable autostart settings were not reported:\n"
                } +
                contents + "\nControls enabled: " +
                (checkbox->isEnabled() ? "true" : "false") + "\nLogs:\n" +
                logs.toStdString()
            };
        }
        gui::set_autostart_enabled(true);
        QMetaObject::invokeMethod(
            tray(window)->contextMenu(), "aboutToShow", Qt::DirectConnection
        );
        require(
            checkbox->isEnabled() && checkbox->isChecked() &&
                action->isEnabled() && action->isChecked(),
            "autostart controls did not recover after the entry was repaired"
        );
    }
    require(
        QFile::remove(gui::autostart_file_path()), "cannot remove test entry"
    );
    std::filesystem::create_directory(path);
    test::write_file(path / "sentinel", "unchanged");
    require(
        test::throws_with<std::runtime_error>(
            [] { gui::set_autostart_enabled(true); },
            "Cannot save the autostart entry"
        ) && test::read_file(path / "sentinel") == "unchanged",
        "failed atomic autostart replacement was hidden or damaged existing "
        "data"
    );
}

void test_autostart_executable_quoting() {
    QTemporaryDir directory;
    require(directory.isValid(), "cannot create isolated autostart directory");
    const auto executable = directory.filePath("client 空 格 \" \\ $ ` %1");
    require(
        QFile::copy(QCoreApplication::applicationFilePath(), executable),
        "cannot copy the autostart helper to a special-character executable "
        "path"
    );
    QProcess child;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("XDG_CONFIG_HOME", directory.path());
    environment.insert("QT_QPA_PLATFORM", "offscreen");
    child.setProcessEnvironment(environment);
    child.start(executable, {"--write-autostart-entry"});
    require(child.waitForStarted(5000), "autostart helper did not start");
    if (!child.waitForFinished(5000)) {
        child.kill();
        child.waitForFinished(5000);
        require(false, "autostart helper did not finish");
    }
    require(
        child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
        "autostart helper failed to write its desktop entry"
    );
    const auto contents = QString::fromStdString(
        test::read_file(directory.filePath("autostart/onedrive-cpp-gui.desktop")
                            .toStdString())
    );
    require(
        contents.contains(
            "Exec=\"" + directory.path() +
            "/client 空 格 \\\\\" \\\\\\\\ \\\\$ \\\\` %%1\" --start-in-tray\n"
        ),
        "desktop entry did not quote spaces, Unicode, quotes, backslashes, "
        "dollars, backticks and percent field codes"
    );
}

void test_tray_folder_failures() {
    TrayEnvironment environment;
    Fixture fixture;
    {
        auto state = gui::load_app_state_view_model(fixture.file);
        state.sync_directory.clear();
        gui::MainWindow window{state};
        opened_folder.clear();
        QMetaObject::invokeMethod(
            tray(window)->contextMenu(), "aboutToShow", Qt::DirectConnection
        );
        auto* action = tray_action(window, "Open sync folder");
        require(
            !action->isEnabled(), "empty sync directory action was enabled"
        );
        action->trigger();
        require(opened_folder.isEmpty(), "empty sync directory was opened");
    }
    gui::MainWindow window{gui::load_app_state_view_model(fixture.file)};
    window.show();
    window.close();
    require(!window.isVisible(), "folder failure test did not start hidden");
    struct FolderResult {
        ~FolderResult() {
            open_folder_succeeds = true;
        }
    } restore;
    open_folder_succeeds = false;
    bool warned = false;
    QTimer::singleShot(0, &window, [&] {
        if (auto* dialog =
                qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            warned = dialog->windowTitle() == "Cannot open sync folder" &&
                     dialog->text().contains(
                         QString::fromStdString(
                             fixture.config.sync_data_directory.string()
                         )
                     );
            dialog->accept();
        }
    });
    tray_action(window, "Open sync folder")->trigger();
    require(
        warned && window.isVisible(),
        "folder open failure did not restore the window and display its path"
    );
}

void test_tray_unsaved_settings() {
    TrayEnvironment environment;
    Fixture fixture;
    gui::MainWindow window{gui::load_app_state_view_model(fixture.file)};
    window.show();
    auto* field = window.findChild<QLineEdit*>("auth.application_id");
    field->setText("edited-client");
    bool prompted = false;
    const auto answer = [&](QMessageBox::StandardButton response) {
        QTimer::singleShot(0, &window, [&, response] {
            auto* dialog =
                qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (dialog && dialog->objectName() == "unsavedSettingsDialog") {
                prompted = true;
                dialog->button(response)->click();
            }
        });
    };
    answer(QMessageBox::Cancel);
    window.close();
    QCoreApplication::processEvents();
    require(
        !prompted && !window.isVisible() && field->text() == "edited-client",
        "hide-to-tray prompted or discarded unsaved settings"
    );
    answer(QMessageBox::Cancel);
    tray_action(window, "Quit")->trigger();
    require(
        prompted && window.isVisible() && tray(window)->isVisible() &&
            config::Config::load(fixture.file).application_id == "test-client",
        "Quit bypassed unsaved-settings cancellation"
    );
    window.close();
    require(
        !window.isVisible(), "cancelled Quit changed subsequent close behavior"
    );
    prompted = false;
    answer(QMessageBox::Save);
    tray_action(window, "Quit")->trigger();
    require(
        prompted && !window.isVisible() && !tray(window)->isVisible() &&
            config::Config::load(fixture.file).application_id ==
                "edited-client",
        "Quit did not save settings before shutting down"
    );
}

void test_tray_graceful_quit() {
    for (const bool signing_in : {false, true}) {
        TrayEnvironment environment;
        Fixture fixture;
        std::atomic<bool> entered{};
        std::atomic<bool> cancelled{};
        std::atomic<bool> finish{};
        const auto work = [&](std::stop_token token) {
            entered = true;
            while (!token.stop_requested()) {
                QThread::msleep(1);
            }
            cancelled = true;
            while (!finish) {
                QThread::msleep(1);
            }
        };
        auto state = gui::load_app_state_view_model(fixture.file);
        if (signing_in) {
            state.account_status = gui::AccountStatus::no_account;
        }
        gui::MainWindow window{
            state,
            [&](
                const auto&, const auto&, const std::stop_token& token
            ) -> auth::AuthResult<app::AuthenticationResult> {
                work(token);
                return std::unexpected(
                    auth::AuthError{.code = auth::AuthErrorCode::cancelled}
                );
            },
            nullptr,
            [&](config::Config,
                const events::Observer&,
                std::stop_token token) {
                work(token);
                return exit_code::cancelled;
            }
        };
        struct FinishWorker {
            std::atomic<bool>& finish;
            ~FinishWorker() {
                finish = true;
            }
        } release{finish};
        window.show();
        button(window, signing_in ? "signInButton" : "startSyncButton")
            ->click();
        require(
            wait_until([&] { return entered.load(); }), "worker did not start"
        );
        window.close();
        QCoreApplication::processEvents();
        require(
            !window.isVisible() && !cancelled,
            "hiding the window cancelled its background worker"
        );
        tray_action(window, "Quit")->trigger();
        require(
            wait_until([&] { return cancelled.load(); }) &&
                window.isVisible() && tray(window)->isVisible(),
            "Quit did not request cancellation and wait for cleanup"
        );
        finish = true;
        bool timed_out = false;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &window, [&] {
            timed_out = true;
            QApplication::quit();
        });
        deadline.start(5000);
        QApplication::exec();
        require(
            !timed_out && !window.isVisible() && !tray(window)->isVisible(),
            "Quit did not exit the application after the worker stopped"
        );
    }
}

} // namespace onedrive::test::gui_sync
