#include "app_state_view_model.hpp"
#include "configuration.hpp"
#include "localization.hpp"
#include "login_controller.hpp"
#include "main_window.hpp"
#include "onedrive/account/account_state.hpp"
#include "support/common.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <sys/stat.h>

class StartupBrowserCapture final : public QObject {
    Q_OBJECT
public:
    int calls{};
public Q_SLOTS:
    void open(const QUrl&) { ++calls; }
};

namespace {

using namespace onedrive;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string configuration(const std::filesystem::path& root) {
    return "config_version = 2\n[sync]\ndata_directory = \"" +
           (root / "sync").string() + "\"\n[state]\ndirectory = \"" +
           (root / "state").string() +
           "\"\n[auth]\napplication_id = \"test-client\"\n";
}

void check_window(
    const gui::AppStateViewModel& state,
    bool sign_in_enabled,
    StartupBrowserCapture& browser
) {
    std::atomic<int> authentication_calls{};
    gui::Localization localization{gui::Language::english};
    gui::MainWindow window{
        state,
        [&](const auto&, const auto&, auto)
            -> auth::AuthResult<app::AuthenticationResult> {
            ++authentication_calls;
            return app::AuthenticationResult{};
        },
        &localization
    };
    window.show();
    QCoreApplication::processEvents();
    const auto* sign_in = window.findChild<QPushButton*>("signInButton");
    const auto* status = window.findChild<QLabel*>("startupStatus");
    const auto* login_status = window.findChild<QLabel*>("loginStatus");
    require(sign_in && status && login_status, "startup controls are missing");
    require(sign_in->isEnabled() == sign_in_enabled,
            "startup sign-in availability is incorrect");
    const auto* logout = window.findChild<QPushButton*>("accountActionButton");
    require(
        logout && logout->text() == "Log out" &&
            logout->isEnabled() ==
                (state.account_status ==
                 gui::AccountStatus::credentials_available) &&
            !window.findChild<QPushButton*>("openBrowserButton"),
        "startup logout availability is incorrect"
    );
    require(window.findChild<QWidget*>("settingsPage")->isEnabled() &&
                window.findChild<QPushButton*>("importSettingsButton")->isEnabled() &&
                window.findChild<QPushButton*>("reloadSettingsButton")->isEnabled(),
            "startup error blocked settings recovery");
    require(
        window.findChild<QPushButton*>("saveSettingsButton")->isEnabled() ==
            (state.configuration_status != gui::ConfigurationStatus::invalid),
        "malformed configuration can be overwritten by basic settings"
    );
    if (state.configuration_status == gui::ConfigurationStatus::invalid) {
        require(status->text().contains(
                    QString::fromStdString(state.configuration_error)),
                "configuration error details were lost");
        require(window.findChild<QTabWidget*>()->currentWidget() ==
                    window.findChild<QWidget*>("settingsPage"),
                "invalid configuration did not open Settings");
    }
    if (state.configuration_status == gui::ConfigurationStatus::missing) {
        require(status->text().contains("Using defaults"),
                "missing configuration was not reported as setup");
    }
    if (state.account_status == gui::AccountStatus::credentials_available) {
        require(login_status->text().contains("not been verified online"),
                "saved credentials were represented as online verified");
    }
    if (!state.account_error.empty()) {
        require(status->text().contains(QString::fromStdString(state.account_error)),
                "local account error details were lost");
    }
    const auto english = status->text();
    localization.set_language(gui::Language::simplified_chinese);
    QCoreApplication::processEvents();
    require(window.findChild<QPushButton*>("reloadSettingsButton")->text() ==
                "重新加载配置",
            "recovery control did not retranslate");
    require(logout->text() == "退出登录", "logout control did not retranslate");
    if (!english.isEmpty()) {
        require(status->text() != english, "startup error did not retranslate");
    }
    if (!state.configuration_error.empty()) {
        require(status->text().contains(
                    QString::fromStdString(state.configuration_error)),
                "runtime translation changed the original parse error");
    }
    localization.set_language(gui::Language::english);
    QCoreApplication::processEvents();
    require(status->text() == english, "startup error failed to switch back");
    require(authentication_calls == 0 && browser.calls == 0 &&
                !QApplication::activeModalWidget(),
            "startup performed authentication, opened a browser or a modal");
}

void test_configuration_startup(StartupBrowserCapture& browser) {
    test::TemporaryDirectory fixture;
    const auto file = fixture.path() / "config.toml";
    auto state = gui::load_app_state_view_model(file);
    require(state.configuration_status == gui::ConfigurationStatus::missing &&
                !std::filesystem::exists(file) &&
                !state.authentication_error.empty(),
            "missing configuration did not retain setup defaults");
    check_window(state, false, browser);
    for (const auto& contents : {
             std::string{"[broken TOML"},
             std::string{"config_version = 1\n"},
             std::string{"config_version = 2\n[console]\nui = \"invalid\"\n"},
         }) {
        test::write_file(file, contents);
        state = gui::load_app_state_view_model(file);
        require(state.configuration_status == gui::ConfigurationStatus::invalid &&
                    state.account_status == gui::AccountStatus::unavailable &&
                    !state.configuration_error.empty() &&
                    test::read_file(file) == contents,
                "invalid configuration was not recoverable and preserved");
        check_window(state, false, browser);
    }
    std::filesystem::remove(file);
    std::filesystem::create_symlink(fixture.path() / "absent", file);
    state = gui::load_app_state_view_model(file);
    require(state.configuration_status == gui::ConfigurationStatus::invalid,
            "dangling configuration link was silently treated as missing");
    check_window(state, false, browser);
    std::filesystem::remove(file);
    test::write_file(file, configuration(fixture.path()));
    state = gui::load_app_state_view_model(file);
    require(state.account_status == gui::AccountStatus::no_account,
            "valid configuration without an account was not logged out");
    check_window(state, true, browser);
    require(!std::filesystem::exists(fixture.path() / "state"),
            "startup created state directories");
    test::write_file(file,
                     "config_version = 2\n[state]\ndirectory = \"\"\n"
                     "[auth]\napplication_id = \"test-client\"\n");
    state = gui::load_app_state_view_model(file);
    require(state.account_status == gui::AccountStatus::invalid,
            "empty state directory was accepted");
    check_window(state, false, browser);
}

void test_account_startup(StartupBrowserCapture& browser) {
    test::TemporaryDirectory fixture;
    const auto file = fixture.path() / "config.toml";
    const auto state_directory = fixture.path() / "state";
    test::write_file(file, configuration(fixture.path()));
    std::filesystem::create_directory(state_directory);
    const auto paths = account::AccountState::activate(
        state_directory,
        {.user_id = "user", .user_display_name = "User",
         .configured_drive_id = "me", .drive_id = "drive",
         .drive_name = "Drive"},
        "saved-token"
    );
    auto state = gui::load_app_state_view_model(file);
    require(state.account_status == gui::AccountStatus::credentials_available,
            "saved credentials were not detected");
    check_window(state, false, browser);
    const auto token = paths.token_directory / "refresh_token";
    std::filesystem::remove(token);
    state = gui::load_app_state_view_model(file);
    require(state.account_status == gui::AccountStatus::no_credentials,
            "absent credentials were not detected");
    check_window(state, true, browser);
    for (const auto contents : {"", " \r\n", "saved-token"}) {
        test::write_file(token, contents);
        require(::chmod(token.c_str(),
                        std::string_view{contents} == "saved-token" ? 0644 : 0600) == 0,
                "cannot set token fixture permissions");
        state = gui::load_app_state_view_model(file);
        require(state.account_status == gui::AccountStatus::invalid,
                "empty or unreadable token was silently treated as logged out");
        check_window(state, false, browser);
    }
    std::filesystem::remove(token);
    std::filesystem::create_symlink(fixture.path() / "absent", token);
    state = gui::load_app_state_view_model(file);
    require(state.account_status == gui::AccountStatus::invalid,
            "dangling token link was silently treated as missing");
    check_window(state, false, browser);
    std::filesystem::remove(token);
    const auto marker = state_directory / "active_account";
    for (const auto contents : {"", "../bad\n", "absent-account\n"}) {
        test::write_file(marker, contents);
        state = gui::load_app_state_view_model(file);
        require(state.account_status == gui::AccountStatus::invalid,
                "invalid active account marker was silently treated as logged out");
        check_window(state, false, browser);
    }
    std::filesystem::remove(marker);
    std::filesystem::create_symlink(fixture.path() / "absent", marker);
    state = gui::load_app_state_view_model(file);
    require(state.account_status == gui::AccountStatus::invalid,
            "unreadable active account marker was silently treated as absent");
    check_window(state, false, browser);
}

void test_import_and_reload_recovery() {
    test::TemporaryDirectory fixture;
    const auto file = fixture.path() / "config.toml";
    const auto source = fixture.path() / "import.toml";
    const std::string corrupt = "# retain this original\n[broken TOML";
    test::write_file(file, corrupt);
    const auto valid =
        "# retain imported comments\n" + configuration(fixture.path()) +
        "tenant_id = 'organizations'\n[upload]\nconcurrency = 7\n"
        "[transfer]\norder = 'size_dsc'\n";
    test::write_file(source, valid);
    gui::MainWindow window{gui::load_app_state_view_model(file)};
    window.show();
    int stage = 0;
    QTimer dialogs;
    QObject::connect(&dialogs, &QTimer::timeout, &window, [&] {
        auto* modal = QApplication::activeModalWidget();
        if (stage == 0) {
            if (auto* dialog = qobject_cast<QFileDialog*>(modal)) {
                dialog->selectFile(QString::fromStdString(source.string()));
                ++stage;
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
        } else if (auto* dialog = qobject_cast<QMessageBox*>(modal)) {
            ++stage;
            if (stage == 2) {
                dialog->button(QMessageBox::Yes)->click();
            } else {
                dialog->accept();
            }
        }
    });
    dialogs.start(1);
    window.findChild<QPushButton*>("importSettingsButton")->click();
    dialogs.stop();
    require(stage == 3 && test::read_file(file) == valid &&
                window.findChild<QPushButton*>("signInButton")->isEnabled() &&
                window.findChild<QPushButton*>("saveSettingsButton")->isEnabled(),
            "GUI import did not recover the active configuration");
    require(
        window.findChild<QLineEdit*>("auth.tenant_id")->text() ==
                "organizations" &&
            window.findChild<QLineEdit*>("upload.concurrency")->text() == "7" &&
            window.findChild<QComboBox*>("transfer.order")->currentData() ==
                "size_dsc",
        "import did not populate all settings sub-tabs"
    );
    bool backed_up = false;
    for (const auto& entry : std::filesystem::directory_iterator(fixture.path())) {
        if (entry.path().filename().string().starts_with("config.toml.backup.")) {
            backed_up = test::read_file(entry.path()) == corrupt;
        }
    }
    require(backed_up, "recovery import did not preserve the corrupt original");
    test::write_file(file, corrupt);
    window.findChild<QPushButton*>("reloadSettingsButton")->click();
    require(!window.findChild<QPushButton*>("signInButton")->isEnabled() &&
                !window.findChild<QPushButton*>("saveSettingsButton")->isEnabled(),
            "reload did not disable operations after external corruption");
    test::write_file(file, valid);
    window.findChild<QPushButton*>("reloadSettingsButton")->click();
    require(window.findChild<QPushButton*>("signInButton")->isEnabled() &&
                window.findChild<QLabel*>("startupStatus")->text().isEmpty(),
            "reload did not recover after external repair");
}

void test_login_independent_of_sync_path() {
    test::TemporaryDirectory fixture;
    const auto file = fixture.path() / "config.toml";
    const auto loop = fixture.path() / "sync";
    std::filesystem::create_symlink(loop, loop);
    test::write_file(file, configuration(fixture.path()));
    const auto state = gui::load_app_state_view_model(file);
    require(!state.sync_error.empty() && state.can_sign_in(),
            "unusable sync path blocked usable authentication configuration");
    int calls = 0;
    bool finished = false;
    gui::LoginController controller{
        [&](const auto&, const auto&, auto)
            -> auth::AuthResult<app::AuthenticationResult> {
            ++calls;
            return app::AuthenticationResult{};
        }
    };
    QString error;
    QObject::connect(&controller, &gui::LoginController::finished,
                     &controller, [&](const QString& detail, bool) {
        error = detail;
        finished = true;
    });
    controller.start(file);
    require(test::wait_until([&] {
        QCoreApplication::processEvents();
        return finished;
    }, std::chrono::seconds{5}) && error.isEmpty() && calls == 1,
            "explicit login still validated the sync path");
}

void test_state_directory_settings_recovery() {
    test::TemporaryDirectory fixture;
    const auto file = fixture.path() / "config.toml";
    test::write_file(file, configuration(fixture.path()));
    std::filesystem::create_directory(fixture.path() / "state");
    const auto marker = fixture.path() / "state/active_account";
    test::write_file(marker, "../invalid-account");
    gui::MainWindow window{gui::load_app_state_view_model(file)};
    window.show();
    require(!window.findChild<QPushButton*>("signInButton")->isEnabled(),
            "corrupt account marker did not disable login");
    const auto new_state = fixture.path() / "recovered-state";
    window.findChild<QLineEdit*>("state.directory")
        ->setText(QString::fromStdString(new_state.string()));
    int stage = 0;
    QTimer dialogs;
    QObject::connect(&dialogs, &QTimer::timeout, &window, [&] {
        auto* dialog = qobject_cast<QMessageBox*>(
            QApplication::activeModalWidget()
        );
        if (!dialog) {
            return;
        }
        ++stage;
        if (stage == 1) {
            dialog->button(QMessageBox::Save)->click();
        } else {
            dialog->accept();
        }
    });
    dialogs.start(1);
    window.findChild<QPushButton*>("saveSettingsButton")->click();
    dialogs.stop();
    require(stage == 2 &&
                config::Config::load(file).state_directory == new_state &&
                window.findChild<QPushButton*>("signInButton")->isEnabled() &&
                window.findChild<QLabel*>("startupStatus")->text().isEmpty() &&
                test::read_file(marker) == "../invalid-account",
            "settings did not recover account state without altering the original");
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication application{argc, argv};
    test::TemporaryDirectory home;
    qputenv("HOME", QByteArray::fromStdString(home.path().string()));
    StartupBrowserCapture browser;
    QDesktopServices::setUrlHandler("https", &browser, "open");
    try {
        test_configuration_startup(browser);
        test_account_startup(browser);
        test_import_and_reload_recovery();
        test_login_independent_of_sync_path();
        test_state_directory_settings_recovery();
    } catch (const std::exception& error) {
        QDesktopServices::unsetUrlHandler("https");
        return test::fail(error.what());
    }
    QDesktopServices::unsetUrlHandler("https");
    return EXIT_SUCCESS;
}

#include "startup_state.moc"
