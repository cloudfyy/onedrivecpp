#include "localization_support.hpp"
#include "localization_cases.hpp"

class BrowserCapture : public QObject {
    Q_OBJECT

public:
    QList<QUrl> addresses;

public Q_SLOTS:
    void open(const QUrl& address) {
        addresses.append(address);
    }
};

namespace onedrive::test::gui_localization {

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
            window.findChild<QPushButton*>("accountActionButton")->click();
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
             QString{
                 "https://login.example.test/authorize?state=%25&code=中文"
             },
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
        auto* account_action =
            window.findChild<QPushButton*>("accountActionButton");
        auto* overview = qobject_cast<QFormLayout*>(
            window.findChild<QTabWidget*>()->widget(0)->layout()
        );
        auto* url = qobject_cast<QLabel*>(
            overview->itemAt(5, QFormLayout::FieldRole)->widget()
        );
        require(
            account_action && url &&
                !window.findChild<QPushButton*>("openBrowserButton"),
            "account action was missing or the obsolete browser button remained"
        );
        sign_in->click();
        const bool valid = address.startsWith("https://login.example.test/");
        if (valid) {
            require(
                wait_until([&] { return browser.addresses.size() == 1; }) &&
                    status->text() == "Waiting for browser authorization (up "
                                      "to 5 minutes)..." &&
                    url->text() ==
                        "Complete authorization in your system browser." &&
                    account_action->isEnabled() &&
                    account_action->text() == "Cancel login",
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
                                          "Waiting for browser authorization "
                                          "(up to 5 minutes)...",
                                          "正在等待浏览器授权（最多 5 分钟）…"
                                      ) &&
                        url->text() == expected(
                                           chinese,
                                           "Complete authorization in your "
                                           "system browser.",
                                           "请在系统浏览器中完成授权。"
                                       ) &&
                        !sign_in->isEnabled() && account_action->isEnabled() &&
                        account_action->text() ==
                            expected(chinese, "Cancel login", "取消登录") &&
                        browser.addresses.size() == 1,
                    "language switching changed browser state or relaunched it"
                );
            }
            account_action->click();
            require(
                !account_action->isEnabled(),
                "cancellation accepted duplicate requests"
            );
        } else {
            require(
                wait_until([&] {
                    return status->text() == "Cannot open browser: the login "
                                             "URL is not a valid HTTPS "
                                             "address.";
                }) &&
                    browser.addresses.isEmpty(),
                "unsafe browser URL was not rejected"
            );
            localization.set_language(gui::Language::simplified_chinese);
            QCoreApplication::processEvents();
            require(
                status->text() == gui::MainWindow::tr(
                                      "Cannot open browser: the login URL is "
                                      "not a valid HTTPS "
                                      "address."
                                  ) &&
                    !sign_in->isEnabled() && browser.addresses.isEmpty(),
                "unsafe URL error did not retranslate without launching a "
                "browser"
            );
            localization.set_language(gui::Language::english);
            QCoreApplication::processEvents();
        }
        finish = true;
        require(
            wait_until([&] { return sign_in->isEnabled(); }) &&
                status->text() == "Login cancelled." && url->text().isEmpty() &&
                !account_action->isEnabled() &&
                account_action->text() == "Log out" &&
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
    enum class RefreshFailure { none, configuration, account, authentication };
    for (const auto failure :
         {RefreshFailure::none,
          RefreshFailure::configuration,
          RefreshFailure::account,
          RefreshFailure::authentication}) {
        test::TemporaryDirectory fixture;
        const auto file = write_config(fixture.path());
        gui::Localization localization{gui::Language::english};
        gui::MainWindow window{
            gui::load_app_state_view_model(file),
            [file, failure](
                const auto& config, const auto&, std::stop_token
            ) -> auth::AuthResult<app::AuthenticationResult> {
                if (failure == RefreshFailure::configuration) {
                    test::write_file(file, "[invalid TOML");
                } else if (failure == RefreshFailure::account) {
                    std::filesystem::create_directories(config.state_directory);
                    std::filesystem::permissions(
                        config.state_directory,
                        std::filesystem::perms::owner_all
                    );
                    test::write_file(
                        config.state_directory / "active_account",
                        "../invalid\n"
                    );
                } else if (failure == RefreshFailure::authentication) {
                    test::write_file(
                        file,
                        test::read_file(file) + "scopes = ['offline_access']\n"
                    );
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
        const bool failed_refresh = failure != RefreshFailure::none;
        require(
            wait_until([&] {
                return window.findChild<QWidget*>("settingsPage")->isEnabled();
            }) &&
                sign_in->isEnabled() == !failed_refresh &&
                sync_field->text() == "/unsaved/sync-中文" &&
                state_field->text() == "/unsaved/state-中文",
            "successful authentication did not complete"
        );
        const auto prefix = QString{
            "Login completed, but account status could not be refreshed: "
        };
        const auto refreshed = gui::load_app_state_view_model(file);
        const auto detail = QString::fromStdString(
            failure == RefreshFailure::configuration
                ? refreshed.configuration_error
            : failure == RefreshFailure::account ? refreshed.account_error
            : failure == RefreshFailure::authentication
                ? refreshed.authentication_error
                : std::string{}
        );
        require(
            failed_refresh
                ? status->text() == prefix + detail && !detail.isEmpty()
                : status->text() == "Signed in. Account credentials saved.",
            "authentication refresh result was not displayed"
        );
        for (const auto language :
             {gui::Language::simplified_chinese, gui::Language::english}) {
            localization.set_language(language);
            QCoreApplication::processEvents();
            const bool chinese = language == gui::Language::simplified_chinese;
            require(
                status->text() ==
                        (failed_refresh
                             ? expected(
                                   chinese,
                                   "Login completed, but account status could "
                                   "not be "
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

} // namespace onedrive::test::gui_localization

#include "localization_login.moc"
