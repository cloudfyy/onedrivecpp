#include "login_controller.hpp"
#include "main_window.hpp"
#include "app_state_view_model.hpp"
#include "support/common.hpp"
#include "onedrive/app/options.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QTcpSocket>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>

#include <atomic>

namespace {

using namespace onedrive;

template <typename Predicate> bool wait_until(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return predicate();
}

int test_loopback() {
    config::Config config;
    config.application_id = "client id";
    config.auth_endpoint = "https://login.example.test";
    QString previous_state;
    QString previous_challenge;
    for (const bool declined : {false, true}) {
        auth::AuthCodeSession session{app::device_auth_options(config)};
        std::stop_source stop;
        std::jthread browser;
        QString authorization_url;
        std::atomic<bool> valid_responses{true};
        const auto result = gui::request_browser_authorization(
            session,
            [&](const QString& address) {
                authorization_url = address;
                browser = std::jthread{[&, address] {
                    const QUrlQuery query{QUrl{address}};
                    const QUrl redirect{
                        query.queryItemValue("redirect_uri", QUrl::FullyDecoded)
                    };
                    auto send = [&](const QByteArray& target,
                                    const QByteArray& host,
                                    bool valid,
                                    const QByteArray& method = "GET",
                                    const QByteArray& version = "HTTP/1.1") {
                        QTcpSocket socket;
                        socket.connectToHost(
                            QHostAddress::LocalHost,
                            static_cast<quint16>(redirect.port())
                        );
                        if (!socket.waitForConnected(2000)) {
                            valid_responses = false;
                            stop.request_stop();
                            return;
                        }
                        socket.write(
                            method + " " + target + " " + version +
                            "\r\nHost: " + host + "\r\n\r\n"
                        );
                        socket.waitForBytesWritten(1000);
                        if (!socket.waitForReadyRead(2000) ||
                            !socket.readAll().startsWith(
                                valid ? "HTTP/1.1 200" : "HTTP/1.1 400"
                            )) {
                            valid_responses = false;
                            stop.request_stop();
                        }
                    };
                    const auto host = redirect.authority().toLatin1();
                    const auto state = query.queryItemValue("state").toLatin1();
                    send("/?state=wrong&code=ignored", host, false);
                    send(
                        "/?state=" + state + "&state=" + state +
                            "&code=ignored",
                        host,
                        false
                    );
                    send(
                        "/wrong?state=" + state + "&code=ignored", host, false
                    );
                    send(
                        "/?state=" + state + "&code=ignored",
                        "evil.example",
                        false
                    );
                    send(
                        "/?state=" + state +
                            "&code=ignored&error=access_denied",
                        host,
                        false
                    );
                    send("/?state=" + state, host, false);
                    send("/?state=" + state + "&code=a&code=b", host, false);
                    send(
                        "http://localhost/?state=" + state + "&code=ignored",
                        host,
                        false
                    );
                    send(
                        "/?state=" + state + "&code=ignored",
                        host + "\r\nHost: " + host,
                        false
                    );
                    send("/?padding=" + QByteArray(8192, 'x'), host, false);
                    send("/", host, false, "POST");
                    send("/", host, false, "GET", "HTTP/1.0");
                    send("/ extra", host, false);
                    send("/%ZZ", host, false);
                    send("//evil.example/", host, false);
                    send("/#fragment", host, false);
                    send("relative", host, false);
                    send(
                        "/?state=" + state +
                            (declined ? "&error=access_denied"
                                      : "&code=test%2Bcode"),
                        host,
                        true
                    );
                }};
            },
            stop.get_token()
        );
        browser.join();
        const QUrlQuery query{QUrl{authorization_url}};
        if (!valid_responses ||
            query.queryItemValue("response_type") != "code" ||
            query.queryItemValue("code_challenge_method") != "S256" ||
            query.queryItemValue("client_id", QUrl::FullyDecoded) !=
                "client id" ||
            query.queryItemValue("state").size() != 43 ||
            query.hasQueryItem("client_secret") ||
            query.queryItemValue("state") == previous_state ||
            query.queryItemValue("code_challenge") == previous_challenge) {
            return test::fail(
                "browser authorization URL or callback validation was incorrect"
            );
        }
        previous_state = query.queryItemValue("state");
        previous_challenge = query.queryItemValue("code_challenge");
        if (declined) {
            if (result || result.error().code !=
                              auth::AuthErrorCode::authorization_declined) {
                return test::fail("browser denial was not reported");
            }
        } else {
            if (!result || session.state() != auth::AuthCodeState::authorized) {
                return test::fail(
                    "browser authorization did not preserve the code, redirect "
                    "or PKCE challenge"
                );
            }
        }
    }
    std::stop_source stop;
    auth::AuthCodeSession cancelled_session{app::device_auth_options(config)};
    const auto cancelled = gui::request_browser_authorization(
        cancelled_session,
        [&](const QString&) { stop.request_stop(); },
        stop.get_token()
    );
    if (cancelled || cancelled.error().code != auth::AuthErrorCode::cancelled) {
        return test::fail("browser callback wait ignored cancellation");
    }
    auth::AuthCodeSession pre_cancelled_session{
        app::device_auth_options(config)
    };
    const auto pre_cancelled = gui::request_browser_authorization(
        pre_cancelled_session,
        [](const QString&) {
            throw std::logic_error{"unexpected browser launch"};
        },
        stop.get_token()
    );
    if (pre_cancelled ||
        pre_cancelled.error().code != auth::AuthErrorCode::cancelled) {
        return test::fail("pre-cancelled browser flow launched a browser");
    }
    std::stop_source slow_stop;
    std::jthread slow_browser;
    QElapsedTimer elapsed;
    elapsed.start();
    auth::AuthCodeSession slow_session{app::device_auth_options(config)};
    const auto interrupted = gui::request_browser_authorization(
        slow_session,
        [&](const QString& address) {
            slow_browser = std::jthread{[&, address] {
                const QUrl redirect{QUrlQuery{QUrl{address}}.queryItemValue(
                    "redirect_uri", QUrl::FullyDecoded
                )};
                QTcpSocket socket;
                socket.connectToHost(
                    QHostAddress::LocalHost,
                    static_cast<quint16>(redirect.port())
                );
                if (socket.waitForConnected(1000)) {
                    socket.write("GET /");
                    socket.waitForBytesWritten(1000);
                    QThread::msleep(100);
                }
                slow_stop.request_stop();
                if (socket.state() == QAbstractSocket::ConnectedState)
                    socket.waitForDisconnected(1000);
            }};
        },
        slow_stop.get_token()
    );
    slow_browser.join();
    if (interrupted ||
        interrupted.error().code != auth::AuthErrorCode::cancelled ||
        elapsed.elapsed() >= 1500) {
        return test::fail(
            "partial callback headers prevented prompt cancellation"
        );
    }
    config.auth_endpoint = "http://login.example.test";
    auth::AuthCodeSession invalid_session{app::device_auth_options(config)};
    const auto invalid = gui::request_browser_authorization(
        invalid_session, [](const QString&) {}
    );
    if (invalid ||
        invalid.error().code != auth::AuthErrorCode::invalid_configuration) {
        return test::fail(
            "browser login accepted an insecure authorization endpoint"
        );
    }
    return EXIT_SUCCESS;
}

std::filesystem::path write_config(const std::filesystem::path& directory) {
    const auto file = directory / "config.toml";
    test::write_file(
        file,
        "config_version = 2\n[sync]\ndata_directory = \"" +
            (directory / "sync").string() + "\"\n[state]\ndirectory = \"" +
            (directory / "state").string() + "\"\n"
    );
    return file;
}

int test_controller() {
    test::TemporaryDirectory temporary;
    const auto file = write_config(temporary.path());
    for (const int outcome : {0, 1, 2, 3}) {
        gui::LoginController controller{
            [outcome](
                const config::Config&, const auto&, std::stop_token token
            ) -> auth::AuthResult<app::AuthenticationResult> {
                if (outcome == 0) {
                    return app::AuthenticationResult{};
                }
                if (outcome == 1) {
                    throw std::runtime_error{"test login failure"};
                }
                while (!token.stop_requested()) {
                    QThread::msleep(1);
                }
                if (outcome == 3) {
                    return app::AuthenticationResult{};
                }
                return std::unexpected(
                    auth::AuthError{.code = auth::AuthErrorCode::cancelled}
                );
            }
        };
        bool finished = false;
        QString error;
        bool cancelled = false;
        QObject::connect(
            &controller,
            &gui::LoginController::finished,
            [&](QString message, bool stopped) {
                finished = true;
                error = message;
                cancelled = stopped;
            }
        );
        controller.start(file);
        if (!controller.running() ||
            !test::throws_with<std::logic_error>(
                [&] { controller.start(file); }, "already running"
            )) {
            return test::fail("login controller accepted concurrent starts");
        }
        if (outcome >= 2) {
            controller.cancel();
        }
        if (!wait_until([&] { return finished; }) || controller.running() ||
            cancelled != (outcome == 2) ||
            (outcome == 1 ? error != "test login failure" : !error.isEmpty())) {
            return test::fail(
                "login controller did not complete on the UI thread with the "
                "expected outcome"
            );
        }
    }
    {
        gui::LoginController controller{
            [](
                const config::Config&, const auto&, std::stop_token token
            ) -> auth::AuthResult<app::AuthenticationResult> {
                while (!token.stop_requested()) {
                    QThread::msleep(1);
                }
                return std::unexpected(
                    auth::AuthError{.code = auth::AuthErrorCode::cancelled}
                );
            }
        };
        controller.start(file);
    }
    QCoreApplication::processEvents();
    return EXIT_SUCCESS;
}

int test_controller_signals_and_restart() {
    test::TemporaryDirectory temporary;
    const auto file = write_config(temporary.path());
    unsigned int attempts = 0;
    gui::LoginController controller{
        [&](
            const config::Config&, const auto& browser, std::stop_token token
        ) -> auth::AuthResult<app::AuthenticationResult> {
            ++attempts;
            browser("https://login.example.test/authorize");
            while (!token.stop_requested()) {
                QThread::msleep(1);
            }
            browser("https://login.example.test/should-not-open");
            return std::unexpected(
                auth::AuthError{
                    .code = auth::AuthErrorCode::server,
                    .message = "authorization failed",
                }
            );
        }
    };
    unsigned int urls = 0;
    unsigned int completions = 0;
    bool correct_thread = true;
    QString error;
    QObject::connect(
        &controller,
        &gui::LoginController::browser_requested,
        &controller,
        [&](const QString& url) {
            correct_thread = correct_thread &&
                             QThread::currentThread() == controller.thread();
            ++urls;
            if (url != "https://login.example.test/authorize")
                correct_thread = false;
            controller.cancel();
        },
        Qt::QueuedConnection
    );
    QObject::connect(
        &controller,
        &gui::LoginController::finished,
        &controller,
        [&](const QString& message, bool cancelled) {
            correct_thread = correct_thread &&
                             QThread::currentThread() == controller.thread() &&
                             !cancelled;
            error = message;
            ++completions;
        }
    );
    for (unsigned int attempt = 1; attempt <= 2; ++attempt) {
        controller.start(file);
        if (!wait_until([&] { return completions == attempt; }) ||
            controller.running() || error != "authorization failed" ||
            urls != attempt || attempts != attempt || !correct_thread) {
            return test::fail(
                "controller restart, queued delivery or post-cancel URL "
                "suppression failed"
            );
        }
    }
    const auto invalid_config = temporary.path() / "invalid.toml";
    test::write_file(invalid_config, "[invalid TOML");
    controller.start(invalid_config);
    if (!wait_until([&] { return completions == 3; }) || error.isEmpty() ||
        attempts != 2 || controller.running()) {
        return test::fail(
            "configuration failure invoked authentication or left controller "
            "running"
        );
    }
    return EXIT_SUCCESS;
}

int test_default_controller() {
    test::TemporaryDirectory temporary;
    const auto file = write_config(temporary.path());
    auto contents = test::read_file(file);
    contents += "\n[auth]\napplication_id = \"test-client\"\n";
    test::write_file(file, contents);
    gui::LoginController controller;
    unsigned int urls = 0;
    bool finished = false;
    bool cancelled = false;
    QString error;
    QObject::connect(
        &controller,
        &gui::LoginController::browser_requested,
        &controller,
        [&](const QString& url) {
            if (QUrlQuery{QUrl{url}}.queryItemValue("response_type") == "code")
                ++urls;
            controller.cancel();
        },
        Qt::QueuedConnection
    );
    QObject::connect(
        &controller,
        &gui::LoginController::finished,
        [&](const QString& message, bool stopped) {
            error = message;
            cancelled = stopped;
            finished = true;
        }
    );
    controller.start(file);
    if (!wait_until([&] { return finished; }) || !cancelled || urls != 1 ||
        controller.running() || error.isEmpty() ||
        account::AccountState::find_active_token_directory(
            temporary.path() / "state"
        )) {
        return test::fail(
            "default production controller did not cancel loopback login "
            "without saving credentials"
        );
    }
    return EXIT_SUCCESS;
}

int test_loopback_timeout_and_invalid_callback() {
    config::Config config;
    config.application_id = "test-client";
    auth::AuthCodeSession missing_callback{app::device_auth_options(config)};
    if (!test::throws_with<std::invalid_argument>(
            [&] {
                static_cast<void>(
                    gui::request_browser_authorization(missing_callback, {})
                );
            },
            "browser callback"
        )) {
        return test::fail(
            "loopback adapter accepted a missing browser callback"
        );
    }
    auth::AuthCodeSession expired{
        app::device_auth_options(config), {}, std::chrono::seconds{1}
    };
    const auto result =
        gui::request_browser_authorization(expired, [](const QString&) {});
    if (result || result.error().code != auth::AuthErrorCode::expired ||
        expired.state() != auth::AuthCodeState::expired) {
        return test::fail("idle loopback listener failed to expire");
    }
    return EXIT_SUCCESS;
}

int test_partial_connections() {
    config::Config config;
    config.application_id = "test-client";
    auth::AuthCodeSession session{app::device_auth_options(config)};
    std::stop_source stop;
    std::jthread browser;
    std::atomic<bool> valid{true};
    const auto result = gui::request_browser_authorization(
        session,
        [&](const QString& address) {
            browser = std::jthread{[&, address] {
                const QUrlQuery query{QUrl{address}};
                const QUrl redirect{
                    query.queryItemValue("redirect_uri", QUrl::FullyDecoded)
                };
                const auto connect = [&](QTcpSocket& socket) {
                    socket.connectToHost(
                        QHostAddress::LocalHost,
                        static_cast<quint16>(redirect.port())
                    );
                    if (!socket.waitForConnected(2000)) {
                        valid = false;
                        stop.request_stop();
                        return false;
                    }
                    return true;
                };
                {
                    QTcpSocket socket;
                    if (!connect(socket))
                        return;
                    socket.write("GET /");
                    socket.waitForBytesWritten(1000);
                    socket.disconnectFromHost();
                    if (socket.state() != QAbstractSocket::UnconnectedState) {
                        socket.waitForDisconnected(1000);
                    }
                }
                {
                    QTcpSocket socket;
                    if (!connect(socket))
                        return;
                    socket.write("GET / HTTP/1.1\r\n");
                    socket.waitForBytesWritten(1000);
                    if (!socket.waitForReadyRead(4000) ||
                        !socket.readAll().startsWith("HTTP/1.1 400")) {
                        valid = false;
                        stop.request_stop();
                        return;
                    }
                }
                QTcpSocket socket;
                if (!connect(socket))
                    return;
                socket.write(
                    "GET /?state=" + query.queryItemValue("state").toLatin1() +
                    "&code=after-partial HTTP/1.1\r\nHost: " +
                    redirect.authority().toLatin1() + "\r\n\r\n"
                );
                socket.waitForBytesWritten(1000);
                if (!socket.waitForReadyRead(2000) ||
                    !socket.readAll().startsWith("HTTP/1.1 200")) {
                    valid = false;
                    stop.request_stop();
                }
            }};
        },
        stop.get_token()
    );
    browser.join();
    if (!result || !valid ||
        session.state() != auth::AuthCodeState::authorized) {
        return test::fail(
            "disconnected or stalled requests prevented subsequent valid "
            "authorization"
        );
    }
    return EXIT_SUCCESS;
}

int test_window_close() {
    test::TemporaryDirectory temporary;
    const auto file = write_config(temporary.path());
    gui::MainWindow window{
        gui::load_app_state_view_model(file),
        [](
            const config::Config&, const auto&, std::stop_token token
        ) -> auth::AuthResult<app::AuthenticationResult> {
            while (!token.stop_requested()) {
                QThread::msleep(1);
            }
            return std::unexpected(
                auth::AuthError{.code = auth::AuthErrorCode::cancelled}
            );
        }
    };
    window.show();
    auto* login = window.findChild<QPushButton*>("signInButton");
    auto* cancel = window.findChild<QPushButton*>("cancelLoginButton");
    if (!login || !cancel) {
        return test::fail("window login controls were missing");
    }
    login->click();
    if (login->isEnabled() || !cancel->isEnabled()) {
        return test::fail("window did not disable duplicate login");
    }
    window.close();
    if (!wait_until([&] { return !window.isVisible(); })) {
        return test::fail(
            "window did not cancel and finish login before closing"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    QApplication application{argc, argv};
    application.setQuitOnLastWindowClosed(false);
    if (test_loopback() != EXIT_SUCCESS || test_controller() != EXIT_SUCCESS ||
        test_controller_signals_and_restart() != EXIT_SUCCESS ||
        test_default_controller() != EXIT_SUCCESS ||
        test_loopback_timeout_and_invalid_callback() != EXIT_SUCCESS ||
        test_partial_connections() != EXIT_SUCCESS ||
        test_window_close() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
