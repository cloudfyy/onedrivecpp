#include "localization_support.hpp"
#include "localization_cases.hpp"

#include <QAction>
#include <QCheckBox>
#include <QMenu>
#include <QSystemTrayIcon>

namespace onedrive::test::gui_localization {

void test_runtime_language(bool system_chinese) {
    test::TemporaryDirectory fixture;
    const auto file = write_config(fixture.path());
    gui::Localization localization{gui::Language::english};
    std::atomic<int> login_calls{0};
    gui::MainWindow window{
        gui::load_app_state_view_model(file),
        [&login_calls](
            const auto&, const auto&, const std::stop_token& token
        ) -> auth::AuthResult<app::AuthenticationResult> {
            ++login_calls;
            while (!token.stop_requested()) {
                QThread::msleep(1);
            }
            return std::unexpected(
                auth::AuthError{.code = auth::AuthErrorCode::cancelled}
            );
        },
        &localization
    };
    window.show();
    const auto fields = window.findChildren<QLineEdit*>();
    require(fields.size() >= 2, "settings fields were not found");
    auto* sync_field = window.findChild<QLineEdit*>("sync.data_directory");
    auto* state_field = window.findChild<QLineEdit*>("state.directory");
    sync_field->setText("/unsaved/同步-%1");
    state_field->setText("/unsaved/状态-%2");
    sync_field->setCursorPosition(5);
    sync_field->setModified(true);
    auto* language_selector = window.findChild<QComboBox*>("languageSelector");
    auto* tabs = window.findChild<QTabWidget*>();
    auto* sign_in = window.findChild<QPushButton*>("signInButton");
    auto* about = window.findChild<QPushButton*>("aboutButton");
    auto* status = window.findChild<QLabel*>("loginStatus");
    auto* header = tabs->cornerWidget(Qt::TopRightCorner);
    require(
        language_selector && language_selector->count() == 3 && about &&
            header && header->isAncestorOf(language_selector) &&
            header->isAncestorOf(about) &&
            window.findChildren<QMenuBar*>().isEmpty(),
        "header controls were missing or a menu bar remained"
    );
    const auto check_ui = [&](bool chinese, gui::Language language) {
        auto* tray = window.findChild<QSystemTrayIcon*>("systemTray");
        const auto actions = tray->contextMenu()->actions();
        require(
            tray->toolTip() ==
                    expected(chinese, "onedrive client", "OneDrive 客户端") &&
                actions.at(0)->text() ==
                    expected(chinese, "Open window", "打开窗口") &&
                actions.at(1)->text() ==
                    expected(chinese, "Open sync folder", "打开同步文件夹") &&
                actions.at(2)->text() == expected(
                                             chinese,
                                             "Start automatically at login",
                                             "登录后自动启动"
                                         ) &&
                window.findChild<QCheckBox*>("startAtLogin")->text() ==
                    actions.at(2)->text() &&
                actions.at(4)->text() == expected(chinese, "Quit", "退出"),
            "tray menu or tooltip did not retranslate"
        );
        require(
            window.windowTitle() ==
                    expected(
                        chinese, "onedrive client *", "OneDrive 客户端 *"
                    ) &&
                QApplication::applicationDisplayName() ==
                    window.windowTitle() &&
                tabs->tabText(0) == expected(chinese, "Overview", "概览") &&
                tabs->tabText(1) == expected(chinese, "Sync", "同步") &&
                tabs->tabText(2) == expected(chinese, "Files", "文件") &&
                tabs->tabText(3) == expected(chinese, "Settings *", "设置 *") &&
                tabs->tabText(4) == expected(chinese, "Logs", "日志") &&
                sign_in->text() == expected(chinese, "Sign in", "登录账号") &&
                has_label(window, expected(chinese, "Account", "账号")) &&
                has_label(window, expected(chinese, "Appearance", "外观")) &&
                has_label(
                    window,
                    gui::account_status_text(gui::AccountStatus::no_account)
                ),
            "existing title, tabs, labels or account status did not retranslate"
        );
        require(
            button(window, expected(chinese, "Save settings", "保存设置")) &&
                button(window, expected(chinese, "Import TOML…", "导入 TOML…")),
            "existing settings buttons did not retranslate"
        );
        for (const auto* browse :
             window.findChildren<QPushButton*>("browseButton")) {
            require(
                browse->text() == expected(chinese, "Browse…", "浏览…"),
                "existing browse button did not retranslate"
            );
        }
        require(
            window.findChildren<QLineEdit*>() == fields &&
                sync_field->text() == "/unsaved/同步-%1" &&
                state_field->text() == "/unsaved/状态-%2" &&
                sync_field->cursorPosition() == 5 && sync_field->isModified(),
            "language switching rebuilt fields or discarded unsaved edits"
        );
        require(
            language_selector->accessibleName() ==
                    expected(chinese, "Language", "语言") &&
                has_label(window, expected(chinese, "Language", "语言")) &&
                language_selector->itemText(0) ==
                    expected(chinese, "System language", "系统语言") &&
                language_selector->itemText(1) ==
                    expected(chinese, "English", "英语") &&
                language_selector->itemText(2) ==
                    expected(chinese, "Simplified Chinese", "简体中文"),
            "language selector did not retranslate"
        );
        require(
            language_selector->currentData().toInt() ==
                static_cast<int>(language),
            "language selector showed the wrong selection"
        );
        for (const int width : {860, 600}) {
            window.resize(width, 520);
            QCoreApplication::processEvents();
            require(
                window.width() == width && header->isVisible() &&
                    language_selector->isVisible() && about->isVisible() &&
                    tabs->rect().contains(header->geometry()) &&
                    !tabs->tabBar()->geometry().intersects(
                        header->geometry()
                    ) &&
                    header->rect().contains(language_selector->geometry()) &&
                    header->rect().contains(about->geometry()) &&
                    tabs->usesScrollButtons(),
                "narrow header hid controls or overlapped tabs"
            );
        }
    };
    const auto select = [&](gui::Language language, bool chinese) {
        language_selector->setCurrentIndex(
            language_selector->findData(static_cast<int>(language))
        );
        QCoreApplication::processEvents();
        check_ui(chinese, language);
    };
    check_ui(false, gui::Language::english);
    select(gui::Language::simplified_chinese, true);
    require(
        status->text() == "登录以连接 OneDrive 账号。",
        "existing initial login status did not retranslate"
    );
    select(gui::Language::english, false);
    select(gui::Language::automatic, system_chinese);
    // Exercise auto/English transitions even when both use English strings.
    select(gui::Language::english, false);
    localization.set_language(gui::Language::automatic);
    QCoreApplication::processEvents();
    check_ui(system_chinese, gui::Language::automatic);
    select(gui::Language::english, false);

    sign_in->click();
    require(
        wait_until([&] { return login_calls == 1; }), "login did not start"
    );
    select(gui::Language::simplified_chinese, true);
    require(
        status->text() == "正在启动浏览器登录…" && !sign_in->isEnabled() &&
            !window.findChild<QWidget*>("settingsPage")->isEnabled() &&
            language_selector->isEnabled() && about->isEnabled() &&
            login_calls == 1,
        "language switching reset the in-progress login or its status"
    );
    bool about_opened = false;
    QTimer::singleShot(0, [&] {
        auto* dialog =
            qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (dialog) {
            about_opened = dialog->objectName() == "aboutDialog";
            dialog->accept();
        }
    });
    about->click();
    require(
        about_opened && login_calls == 1 && !sign_in->isEnabled(),
        "header About button interrupted login or failed to open"
    );
    window.findChild<QPushButton*>("accountActionButton")->click();
    select(gui::Language::english, false);
    require(
        wait_until([&] { return sign_in->isEnabled(); }) &&
            status->text() == "Login cancelled." && login_calls == 1,
        "login result was lost after language switching"
    );
    select(gui::Language::simplified_chinese, true);
    require(
        status->text() == "登录已取消。",
        "completed login status did not retranslate"
    );
    select(gui::Language::english, false);

    gui::MainWindow failed_window{
        gui::load_app_state_view_model(file),
        [](
            const auto&, const auto&, const std::stop_token&
        ) -> auth::AuthResult<app::AuthenticationResult> {
            throw std::runtime_error{"raw server detail: /中文/%1"};
        },
        &localization
    };
    auto* failed_sign_in =
        failed_window.findChild<QPushButton*>("signInButton");
    auto* failed_status = failed_window.findChild<QLabel*>("loginStatus");
    failed_sign_in->click();
    require(
        wait_until([&] { return failed_sign_in->isEnabled(); }) &&
            failed_status->text() ==
                "Login failed: raw server detail: /中文/%1",
        "login failure did not retain backend detail"
    );
    select(gui::Language::simplified_chinese, true);
    require(
        failed_status->text() == "登录失败：raw server detail: /中文/%1",
        "existing failure status did not retranslate or preserve backend detail"
    );
    select(gui::Language::english, false);
    require(
        failed_status->text() == "Login failed: raw server detail: /中文/%1",
        "failure status did not restore English with its backend detail"
    );
}

} // namespace onedrive::test::gui_localization
