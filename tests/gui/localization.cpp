#include "localization.hpp"
#include "main_window.hpp"
#include "onedrive/app/options.hpp"
#include "support/common.hpp"

#include <QApplication>
#include <QAction>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <atomic>
#include <stdexcept>
#include <string_view>

class BrowserCapture : public QObject {
    Q_OBJECT

public:
    QList<QUrl> addresses;

public Q_SLOTS:
    void open(const QUrl& address) {
        addresses.append(address);
    }
};

namespace {

using namespace onedrive;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

QString expected(bool chinese, const char* english, const char* translation) {
    return QString::fromUtf8(chinese ? translation : english);
}

template <typename Predicate> bool wait_until(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return predicate();
}

bool has_label(const QWidget& widget, const QString& text) {
    for (const auto* label : widget.findChildren<QLabel*>()) {
        if (label->text() == text) {
            return true;
        }
    }
    return false;
}

QPushButton* button(QWidget& widget, const QString& text) {
    for (auto* candidate : widget.findChildren<QPushButton*>()) {
        if (candidate->text() == text) {
            return candidate;
        }
    }
    throw std::runtime_error{"missing translated button"};
}

std::filesystem::path write_config(const std::filesystem::path& directory) {
    const auto file = directory / "config.toml";
    test::write_file(
        file,
        "config_version = 2\n[sync]\ndata_directory = \"" +
            (directory / "sync-%2-中文").string() +
            "\"\n[state]\ndirectory = \"" +
            (directory / "state-%3-中文").string() +
            "\"\n[auth]\napplication_id = \"test-client\"\n"
    );
    return file;
}

void test_states_and_messages(bool chinese) {
    require(
        QFile::exists(":/i18n/onedrive_zh_CN.qm") &&
            QFile::exists(":/i18n/qtbase_zh_CN.qm"),
        "translations were not embedded in the executable"
    );
    require(
        gui::account_status_text(gui::AccountStatus::no_account) ==
                expected(chinese, "No active account", "无活动账号") &&
            gui::account_status_text(
                gui::AccountStatus::credentials_available
            ) ==
                expected(
                    chinese, "Account credentials available", "账号凭据可用"
                ) &&
            gui::account_status_text(gui::AccountStatus::no_credentials) ==
                expected(
                    chinese,
                    "Active account has no saved credentials",
                    "活动账号没有已保存的凭据"
                ),
        "account state translations were incorrect"
    );
    require(
        gui::sync_mode_text(sync::SyncMode::bidirectional) ==
                expected(chinese, "Bidirectional", "双向同步") &&
            gui::sync_mode_text(sync::SyncMode::upload_only) ==
                expected(chinese, "Upload only", "仅上传") &&
            gui::sync_mode_text(sync::SyncMode::download_only) ==
                expected(chinese, "Download only", "仅下载") &&
            gui::delete_policy_text(sync::DeletePolicy::propagate) ==
                expected(chinese, "Propagate deletions", "传播删除操作") &&
            gui::delete_policy_text(sync::DeletePolicy::preserve) ==
                expected(chinese, "Preserve destination files", "保留目标文件"),
        "configuration enum translations were incorrect"
    );
    const auto detail = QString::fromUtf8("server detail: /配置/%2");
    require(
        gui::MainWindow::tr("Login failed: %1").arg(detail) ==
                expected(chinese, "Login failed: ", "登录失败：") + detail &&
            QCoreApplication::translate(
                "Application", "Cannot start OneDrive client:\n%1"
            )
                    .arg(detail) == expected(
                                        chinese,
                                        "Cannot start OneDrive client:\n",
                                        "无法启动 OneDrive 客户端：\n"
                                    ) + detail,
        "error translation did not preserve backend detail"
    );
    require(
        gui::MainWindow::tr(
            "\n\nSync directory:\n%1\n→ %2\n"
            "Inspect a dry run before the next synchronization."
        )
                .arg("/old/%2-目录", "/new/%1-目录") ==
            expected(
                chinese,
                "\n\nSync directory:\n/old/%2-目录\n→ /new/%1-目录\n"
                "Inspect a dry run before the next synchronization.",
                "\n\n同步目录：\n/old/%2-目录\n→ /new/%1-目录\n"
                "下次同步前请检查试运行结果。"
            ),
        "directory confirmation placeholders were incorrect"
    );
    require(
        gui::MainWindow::tr(
            "Import this configuration to the active user configuration?\n\n"
            "Sync directory: %1\nState directory: %2\nSync mode: %3\n"
            "Deletion policy: %4\n\n"
            "Relative file references will be resolved from the active "
            "configuration directory. The current configuration will be "
            "backed up."
        )
                .arg(
                    "/sync/中文",
                    "/state/中文",
                    gui::sync_mode_text(sync::SyncMode::upload_only),
                    gui::delete_policy_text(sync::DeletePolicy::preserve)
                ) ==
            expected(
                chinese,
                "Import this configuration to the active user "
                "configuration?\n\n"
                "Sync directory: /sync/中文\nState directory: /state/中文\n"
                "Sync mode: Upload only\nDeletion policy: Preserve destination "
                "files\n\n"
                "Relative file references will be resolved from the active "
                "configuration directory. The current configuration will be "
                "backed up.",
                "将此配置导入到当前用户配置吗？\n\n"
                "同步目录：/sync/中文\n状态目录：/state/中文\n"
                "同步模式：仅上传\n删除策略：保留目标文件\n\n"
                "相对文件引用将基于当前配置目录解析。当前配置将被备份。"
            ),
        "import confirmation placeholders were incorrect"
    );
    require(
        gui::MainWindow::tr("TOML files (*.toml);;All files (*)") ==
            expected(
                chinese,
                "TOML files (*.toml);;All files (*)",
                "TOML 文件 (*.toml);;所有文件 (*)"
            ),
        "file dialog filters were not translated"
    );
}

void test_window_and_settings(bool chinese) {
    test::TemporaryDirectory fixture;
    const auto file = write_config(fixture.path());
    const auto state = gui::load_app_state_view_model(file);
    gui::MainWindow window{state};
    require(
        window.windowTitle() ==
            expected(chinese, "onedrive client", "OneDrive 客户端"),
        "window title was not localized or changed the English title"
    );
    const auto* tabs = window.findChild<QTabWidget*>();
    require(
        tabs && tabs->tabText(0) == expected(chinese, "Overview", "概览") &&
            tabs->tabText(1) == expected(chinese, "Settings", "设置"),
        "tab labels were not translated"
    );
    for (const auto& pair : {
             std::pair{"Account", "账号"},
             std::pair{"Configuration file", "配置文件"},
             std::pair{"Sync directory", "同步目录"},
             std::pair{"State directory", "状态目录"},
             std::pair{"Login", "登录"},
             std::pair{"Browser login", "浏览器登录"},
             std::pair{"Appearance", "外观"},
             std::pair{
                 "Follow system light/dark appearance", "跟随系统浅色／深色外观"
             },
         }) {
        require(
            has_label(window, expected(chinese, pair.first, pair.second)),
            "a form label was not translated"
        );
    }
    require(
        has_label(window, QString::fromStdString(file.string())) &&
            has_label(
                window, gui::account_status_text(gui::AccountStatus::no_account)
            ),
        "window changed a path or did not render the translated account state"
    );
    for (const auto& pair : {
             std::pair{"Sign in", "登录账号"},
             std::pair{"Open browser", "打开浏览器"},
             std::pair{"Cancel login", "取消登录"},
             std::pair{"Browse…", "浏览…"},
             std::pair{"Import TOML…", "导入 TOML…"},
         }) {
        static_cast<void>(
            button(window, expected(chinese, pair.first, pair.second))
        );
    }
    auto* save = button(window, expected(chinese, "Save settings", "保存设置"));
    bool dialog_seen = false;
    bool dialog_correct = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog =
            qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        dialog_seen = dialog != nullptr;
        if (dialog) {
            dialog_correct =
                dialog->windowTitle() ==
                    expected(chinese, "Settings saved", "设置已保存") &&
                dialog->text().startsWith(expected(
                    chinese,
                    "Settings saved. Changes take effect on the next "
                    "operation.\nPrevious configuration backed up "
                    "to:\n",
                    "设置已保存。更改将在下次操作时生效。\n原配置已备份到：\n"
                )) &&
                dialog->text().contains(
                    QString::fromStdString(file.string()) + ".backup."
                ) &&
                (chinese
                     ? dialog->button(QMessageBox::Ok)->text().contains("确定")
                     : dialog->button(QMessageBox::Ok)->text().contains("OK"));
            dialog->accept();
        }
    });
    save->click();
    require(
        dialog_seen && dialog_correct,
        "saved settings dialog or Qt buttons were not localized"
    );
    auto* sync_field = window.findChild<QLineEdit*>("sync.data_directory");
    const auto previous = test::read_file(file);
    sync_field->setText("/changed/同步");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog =
            qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        dialog_correct =
            dialog &&
            dialog->windowTitle() ==
                expected(
                    chinese, "Confirm directory changes", "确认目录更改"
                ) &&
            dialog->text().contains(
                QString::fromStdString(state.sync_directory.string())
            ) &&
            dialog->text().contains("/changed/同步") &&
            (!chinese ||
             (dialog->button(QMessageBox::Save)->text().contains("保存") &&
              dialog->button(QMessageBox::Cancel)->text().contains("取消")));
        if (dialog) {
            dialog->button(QMessageBox::Cancel)->click();
        }
    });
    save->click();
    require(
        dialog_correct && test::read_file(file) == previous,
        "directory confirmation was not localized or cancel saved changes"
    );
    sync_field->setText(QString::fromStdString(state.sync_directory.string()));
    const auto imported_file = fixture.path() / "import.toml";
    test::write_file(
        imported_file,
        "config_version = 2\n[sync]\ndata_directory = \"/import/%3-同步\"\n"
        "mode = \"upload_only\"\ndelete_policy = \"preserve\"\n"
        "[state]\ndirectory = \"/import/%4-状态\"\n"
    );
    for (const bool confirm : {false, true}) {
        int stage = 0;
        bool import_correct = true;
        QTimer dialogs;
        QObject::connect(&dialogs, &QTimer::timeout, &window, [&] {
            auto* modal = QApplication::activeModalWidget();
            if (stage == 0) {
                auto* dialog = qobject_cast<QFileDialog*>(modal);
                if (!dialog) {
                    return;
                }
                import_correct =
                    dialog->windowTitle() ==
                        expected(chinese, "Import configuration", "导入配置") &&
                    dialog->nameFilters().first() ==
                        expected(
                            chinese, "TOML files (*.toml)", "TOML 文件 (*.toml)"
                        );
                dialog->selectFile(
                    QString::fromStdString(imported_file.string())
                );
                ++stage;
                QMetaObject::invokeMethod(
                    dialog, "accept", Qt::QueuedConnection
                );
                return;
            }
            auto* dialog = qobject_cast<QMessageBox*>(modal);
            if (!dialog) {
                return;
            }
            if (stage == 1) {
                import_correct =
                    import_correct &&
                    dialog->windowTitle() ==
                        expected(chinese, "Import configuration", "导入配置") &&
                    dialog->text().contains("/import/%3-同步") &&
                    dialog->text().contains("/import/%4-状态") &&
                    dialog->text().contains(
                        expected(chinese, "Upload only", "仅上传")
                    ) &&
                    dialog->text().contains(expected(
                        chinese, "Preserve destination files", "保留目标文件"
                    ));
                ++stage;
                dialog->button(confirm ? QMessageBox::Yes : QMessageBox::Cancel)
                    ->click();
            } else if (stage == 2 && confirm) {
                import_correct =
                    import_correct &&
                    dialog->windowTitle() ==
                        expected(
                            chinese, "Configuration imported", "配置已导入"
                        ) &&
                    dialog->text().startsWith(expected(
                        chinese, "Configuration imported.", "配置已导入。"
                    )) &&
                    dialog->text().contains(
                        QString::fromStdString(file.string()) + ".backup."
                    );
                ++stage;
                dialog->accept();
            }
        });
        dialogs.start(1);
        button(window, expected(chinese, "Import TOML…", "导入 TOML…"))
            ->click();
        dialogs.stop();
        require(
            import_correct && stage == (confirm ? 3 : 2) &&
                test::read_file(file) ==
                    (confirm ? test::read_file(imported_file) : previous),
            "import dialogs lost translations or placeholders, or cancellation "
            "changed configuration"
        );
    }
    test::write_file(file, "[invalid TOML");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog =
            qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        dialog_correct =
            dialog &&
            dialog->windowTitle() ==
                expected(chinese, "Cannot save settings", "无法保存设置") &&
            dialog->text().startsWith(expected(
                chinese, "Settings could not be saved:\n", "设置保存失败：\n"
            )) &&
            dialog->text().size() > 40;
        if (dialog) {
            dialog->accept();
        }
    });
    save->click();
    require(
        dialog_correct,
        "settings error did not retain translated context and backend detail"
    );
}

void test_login(bool chinese) {
    test::TemporaryDirectory fixture;
    const auto file = write_config(fixture.path());
    for (const int outcome : {0, 1, 2}) {
        gui::MainWindow window{
            gui::load_app_state_view_model(file),
            [outcome](
                const auto&, const auto&, std::stop_token token
            ) -> auth::AuthResult<app::AuthenticationResult> {
                if (outcome == 0) {
                    return app::AuthenticationResult{};
                }
                if (outcome == 1) {
                    throw std::runtime_error{"raw server detail"};
                }
                while (!token.stop_requested()) {
                    QThread::msleep(1);
                }
                return std::unexpected(
                    auth::AuthError{.code = auth::AuthErrorCode::cancelled}
                );
            }
        };
        auto* status = window.findChild<QLabel*>("loginStatus");
        auto* sign_in = window.findChild<QPushButton*>("signInButton");
        require(
            status->text() == expected(
                                  chinese,
                                  "Sign in to connect a OneDrive account.",
                                  "登录以连接 OneDrive 账号。"
                              ),
            "initial login status was not translated"
        );
        sign_in->click();
        require(
            status->text() ==
                expected(
                    chinese, "Starting browser login...", "正在启动浏览器登录…"
                ),
            "starting login status was not translated"
        );
        if (outcome == 2) {
            window.findChild<QPushButton*>("cancelLoginButton")->click();
            require(
                status->text() ==
                    expected(chinese, "Cancelling login...", "正在取消登录…"),
                "cancelling login status was not translated"
            );
        }
        require(
            wait_until([&] { return sign_in->isEnabled(); }),
            "login did not finish"
        );
        const auto message =
            outcome == 0 ? expected(
                               chinese,
                               "Signed in. Account credentials saved.",
                               "已登录。账号凭据已保存。"
                           )
            : outcome == 1
                ? expected(
                      chinese,
                      "Login failed: raw server detail",
                      "登录失败：raw server detail"
                  )
                : expected(chinese, "Login cancelled.", "登录已取消。");
        require(
            status->text() == message,
            "completed login status was not translated"
        );
    }
}

void test_browser_callback(bool chinese) {
    config::Config config;
    config.application_id = "test-client";
    auth::AuthCodeSession session{app::device_auth_options(config)};
    std::jthread browser;
    std::stop_source stop;
    std::atomic<bool> correct{true};
    const auto result = gui::request_browser_authorization(
        session,
        [&](const QString& address) {
            browser = std::jthread{[&, address] {
                const QUrlQuery query{QUrl{address}};
                const QUrl redirect{
                    query.queryItemValue("redirect_uri", QUrl::FullyDecoded)
                };
                for (const bool valid : {false, true}) {
                    QTcpSocket socket;
                    socket.connectToHost(
                        QHostAddress::LocalHost,
                        static_cast<quint16>(redirect.port())
                    );
                    if (!socket.waitForConnected(2000)) {
                        correct = false;
                        stop.request_stop();
                        return;
                    }
                    const auto state =
                        valid ? query.queryItemValue("state").toLatin1()
                              : QByteArray{"invalid"};
                    socket.write(
                        "GET /?state=" + state +
                        "&code=test-code HTTP/1.1\r\nHost: " +
                        redirect.authority().toLatin1() + "\r\n\r\n"
                    );
                    socket.waitForBytesWritten(1000);
                    QByteArray response;
                    while (socket.waitForReadyRead(2000)) {
                        response += socket.readAll();
                    }
                    const auto body =
                        valid ? expected(
                                    chinese,
                                    "Authorization received. Return to "
                                    "OneDrive client. You may close this tab.",
                                    "已收到授权。请返回 OneDrive "
                                    "客户端。可以关闭此标签页。"
                                )
                                    .toUtf8()
                              : expected(
                                    chinese,
                                    "Invalid authorization callback.",
                                    "无效的授权回调。"
                                )
                                    .toUtf8();
                    if (!response.endsWith("\r\n\r\n" + body) ||
                        !response.contains(
                            "Content-Length: " +
                            QByteArray::number(body.size()) + "\r\n"
                        )) {
                        correct = false;
                        stop.request_stop();
                        return;
                    }
                }
            }};
        },
        stop.get_token()
    );
    browser.join();
    require(
        result.has_value() && correct,
        "browser callback text or UTF-8 length was incorrect"
    );
    auth::AuthCodeSession missing{app::device_auth_options(config)};
    try {
        static_cast<void>(gui::request_browser_authorization(missing, {}));
        require(false, "missing browser callback did not fail");
    } catch (const std::invalid_argument& error) {
        require(
            QString::fromUtf8(error.what()) ==
                expected(
                    chinese,
                    "Browser login requires a browser callback.",
                    "浏览器登录需要浏览器回调。"
                ),
            "browser adapter error was not translated"
        );
    }
}

void test_runtime_language(bool system_chinese) {
    test::TemporaryDirectory fixture;
    const auto file = write_config(fixture.path());
    gui::Localization localization{gui::Language::english};
    std::atomic<int> login_calls{0};
    gui::MainWindow window{
        gui::load_app_state_view_model(file),
        [&login_calls](
            const auto&, const auto&, std::stop_token token
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
    auto* menu = window.findChild<QMenu*>("languageMenu");
    auto* tabs = window.findChild<QTabWidget*>();
    auto* sign_in = window.findChild<QPushButton*>("signInButton");
    auto* status = window.findChild<QLabel*>("loginStatus");
    require(menu && menu->actions().size() == 3, "language menu was missing");
    const auto check_ui = [&](bool chinese, gui::Language language) {
        require(
            window.windowTitle() ==
                    expected(chinese, "onedrive client", "OneDrive 客户端") &&
                QApplication::applicationDisplayName() ==
                    window.windowTitle() &&
                tabs->tabText(0) == expected(chinese, "Overview", "概览") &&
                tabs->tabText(1) == expected(chinese, "Settings", "设置") &&
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
            menu->title() == expected(chinese, "Language", "语言") &&
                menu->actions()[0]->text() ==
                    expected(chinese, "System language", "系统语言") &&
                menu->actions()[1]->text() ==
                    expected(chinese, "English", "英语") &&
                menu->actions()[2]->text() ==
                    expected(chinese, "Simplified Chinese", "简体中文"),
            "language menu did not retranslate"
        );
        for (auto* action : menu->actions()) {
            require(
                action->isChecked() ==
                    (action->data().toInt() == static_cast<int>(language)),
                "language menu checked the wrong selection"
            );
        }
    };
    const auto select = [&](gui::Language language, bool chinese) {
        menu->actions()[static_cast<int>(language)]->trigger();
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
            login_calls == 1,
        "language switching reset the in-progress login or its status"
    );
    window.findChild<QPushButton*>("cancelLoginButton")->click();
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
        [](const auto&,
           const auto&,
           std::stop_token) -> auth::AuthResult<app::AuthenticationResult> {
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

void test_browser_language() {
    test::TemporaryDirectory fixture;
    const auto file = write_config(fixture.path());
    gui::Localization localization{gui::Language::english};
    BrowserCapture browser;
    QDesktopServices::setUrlHandler("https", &browser, "open");
    struct ResetHandler {
        ~ResetHandler() {
            QDesktopServices::unsetUrlHandler("https");
        }
    } reset_handler;
    for (const auto& address : {
             QString{"https://login.example.test/authorize?state=%25&code=中文"},
             QString{"https://["},
             QString{"http://login.example.test/"},
             QString{"https:///authorize"},
             QString{"https://user:secret@login.example.test/"},
         }) {
        localization.set_language(gui::Language::english);
        browser.addresses.clear();
        std::atomic<bool> finish{false};
        gui::MainWindow window{
            gui::load_app_state_view_model(file),
            [address, &finish](
                const auto&, const auto& open, std::stop_token token
            ) -> auth::AuthResult<app::AuthenticationResult> {
                open(address);
                while (!token.stop_requested()) {
                    QThread::msleep(1);
                }
                while (!finish) {
                    QThread::msleep(1);
                }
                return std::unexpected(
                    auth::AuthError{.code = auth::AuthErrorCode::cancelled}
                );
            },
            &localization
        };
        struct ReleaseWorker {
            std::atomic<bool>& finish;
            ~ReleaseWorker() {
                finish = true;
            }
        } release_worker{finish};
        auto* sign_in = window.findChild<QPushButton*>("signInButton");
        auto* status = window.findChild<QLabel*>("loginStatus");
        auto* open = window.findChild<QPushButton*>("openBrowserButton");
        auto* overview = qobject_cast<QFormLayout*>(
            window.findChild<QTabWidget*>()->widget(0)->layout()
        );
        auto* url = qobject_cast<QLabel*>(
            overview->itemAt(5, QFormLayout::FieldRole)->widget()
        );
        require(open && url, "browser controls were missing");
        sign_in->click();
        const bool valid = address.startsWith(
            "https://login.example.test/"
        );
        if (valid) {
            require(
                wait_until([&] { return browser.addresses.size() == 1; }) &&
                    status->text() ==
                        "Waiting for browser authorization (up to 5 minutes)..." &&
                    url->text() ==
                        "Complete authorization in your system browser." &&
                    open->isEnabled(),
                "browser request did not display its instruction and status"
            );
            for (const auto language :
                 {gui::Language::simplified_chinese, gui::Language::english}) {
                localization.set_language(language);
                QCoreApplication::processEvents();
                const bool chinese =
                    language == gui::Language::simplified_chinese;
                require(
                    status->text() == expected(
                        chinese,
                        "Waiting for browser authorization (up to 5 minutes)...",
                        "正在等待浏览器授权（最多 5 分钟）…"
                    ) &&
                        url->text() == expected(
                            chinese,
                            "Complete authorization in your system browser.",
                            "请在系统浏览器中完成授权。"
                        ) &&
                        !sign_in->isEnabled() && open->isEnabled() &&
                        browser.addresses.size() == 1,
                    "language switching changed browser state or relaunched it"
                );
            }
            open->click();
            require(
                browser.addresses.size() == 2 &&
                    browser.addresses[0] == QUrl{address, QUrl::StrictMode} &&
                    browser.addresses[1] == browser.addresses[0],
                "reopening the browser changed the authorization URL"
            );
            window.findChild<QPushButton*>("cancelLoginButton")->click();
        } else {
            require(
                wait_until([&] {
                    return status->text() ==
                        "Cannot open browser: the login URL is not a valid HTTPS "
                        "address.";
                }) && browser.addresses.isEmpty(),
                "unsafe browser URL was not rejected"
            );
            localization.set_language(gui::Language::simplified_chinese);
            QCoreApplication::processEvents();
            require(
                status->text() ==
                    gui::MainWindow::tr(
                        "Cannot open browser: the login URL is not a valid HTTPS "
                        "address."
                    ) &&
                    !sign_in->isEnabled() && browser.addresses.isEmpty(),
                "unsafe URL error did not retranslate without launching a browser"
            );
            localization.set_language(gui::Language::english);
            QCoreApplication::processEvents();
        }
        finish = true;
        require(
            wait_until([&] { return sign_in->isEnabled(); }) &&
                status->text() == "Login cancelled." && url->text().isEmpty() &&
                !open->isEnabled() &&
                (valid || browser.addresses.isEmpty()),
            "browser completion retained stale controls or opened an unsafe URL"
        );
        localization.set_language(gui::Language::simplified_chinese);
        QCoreApplication::processEvents();
        require(
            status->text() == "登录已取消。" && url->text().isEmpty(),
            "language switching resurrected a cleared browser instruction"
        );
    }
}

void test_login_refresh_language() {
    for (const bool invalid_config : {false, true}) {
        test::TemporaryDirectory fixture;
        const auto file = write_config(fixture.path());
        gui::Localization localization{gui::Language::english};
        gui::MainWindow window{
            gui::load_app_state_view_model(file),
            [file, invalid_config](
                const auto&, const auto&, std::stop_token
            ) -> auth::AuthResult<app::AuthenticationResult> {
                if (invalid_config) {
                    test::write_file(file, "[invalid TOML");
                }
                return app::AuthenticationResult{};
            },
            &localization
        };
        auto* sign_in = window.findChild<QPushButton*>("signInButton");
        auto* status = window.findChild<QLabel*>("loginStatus");
        auto* sync_field = window.findChild<QLineEdit*>("sync.data_directory");
        auto* state_field = window.findChild<QLineEdit*>("state.directory");
        sync_field->setText("/unsaved/sync-中文");
        state_field->setText("/unsaved/state-中文");
        sign_in->click();
        require(
            wait_until([&] {
                return window.findChild<QWidget*>("settingsPage")->isEnabled();
            }) &&
                sign_in->isEnabled() == !invalid_config &&
                sync_field->text() == "/unsaved/sync-中文" &&
                state_field->text() == "/unsaved/state-中文",
            "successful authentication did not complete"
        );
        const auto prefix = QString{
            "Login completed, but account status could not be refreshed: "
        };
        const auto detail = invalid_config ? status->text().mid(prefix.size())
                                           : QString{};
        require(
            invalid_config
                ? status->text().startsWith(prefix) && !detail.isEmpty()
                : status->text() == "Signed in. Account credentials saved.",
            "authentication refresh result was not displayed"
        );
        for (const auto language :
             {gui::Language::simplified_chinese, gui::Language::english}) {
            localization.set_language(language);
            QCoreApplication::processEvents();
            const bool chinese =
                language == gui::Language::simplified_chinese;
            require(
                status->text() ==
                    (invalid_config
                         ? expected(
                               chinese,
                               "Login completed, but account status could not be "
                               "refreshed: ",
                               "登录已完成，但无法刷新账号状态："
                           ) + detail
                         : expected(
                               chinese,
                               "Signed in. Account credentials saved.",
                               "已登录。账号凭据已保存。"
                           )) &&
                    window.findChild<QWidget*>("settingsPage")->isEnabled(),
                "completed login did not retranslate or preserve refresh detail"
            );
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    test::TemporaryDirectory settings;
    QSettings::setPath(
        QSettings::NativeFormat,
        QSettings::UserScope,
        QString::fromStdString(settings.path().string())
    );
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application{argc, argv};
    const bool chinese = argc > 1 && std::string_view{argv[1]} == "chinese";
    try {
        {
            auto arguments = application.arguments();
            if (arguments.size() > 1) {
                arguments.removeAt(1);
            }
            QCommandLineParser parser;
            gui::Localization localization{
                gui::parse_startup_language(parser, arguments)
            };
            require(
                localization.chinese_loaded() == chinese,
                "startup locale selected the wrong translation"
            );
            test_states_and_messages(chinese);
            test_window_and_settings(chinese);
            test_login(chinese);
            test_browser_callback(chinese);
        }
        require(
            gui::MainWindow::tr("onedrive client") == "onedrive client",
            "destroyed translator remained installed"
        );
        {
            gui::Localization localization{QLocale{"zh_CN"}};
            require(
                localization.chinese_loaded() &&
                    gui::MainWindow::tr("onedrive client") == "OneDrive 客户端",
                "explicit Simplified Chinese translator did not load"
            );
        }
        gui::Localization fallback{QLocale{"en_US"}};
        require(
            !fallback.chinese_loaded() &&
                gui::MainWindow::tr("onedrive client") == "onedrive client",
            "English fallback was not restored"
        );
        const auto system_locale = QLocale::system();
        test_runtime_language(
            system_locale.language() == QLocale::Chinese &&
            system_locale.script() == QLocale::SimplifiedChineseScript
        );
        test_browser_language();
        test_login_refresh_language();
    } catch (const std::exception& error) {
        return test::fail(error.what());
    }

    return EXIT_SUCCESS;
}

#include "localization.moc"
