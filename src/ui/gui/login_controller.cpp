#include "login_controller.hpp"

#include "onedrive/app/factory.hpp"

#include <QMetaObject>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

namespace onedrive::gui {
auth::AuthResult<void> request_browser_authorization(
    auth::AuthCodeSession& session,
    const std::function<void(const QString&)>& open_browser,
    std::stop_token token
) {
    if (!open_browser) {
        throw std::invalid_argument{
            "browser login requires a browser callback"
        };
    }
    if (token.stop_requested()) {
        const auto cancelled = session.begin("http://localhost/", token);
        return std::unexpected(cancelled.error());
    }
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        throw std::runtime_error{
            "cannot listen for browser authorization: " +
            server.errorString().toStdString()
        };
    }
    const auto redirect =
        QString{"http://localhost:%1/"}.arg(server.serverPort());
    const auto url = session.begin(redirect.toStdString(), token);
    if (!url) {
        return std::unexpected(url.error());
    }
    open_browser(QString::fromStdString(*url));
    while (true) {
        if (const auto ready = session.check(token); !ready) {
            return ready;
        }
        bool timed_out = false;
        if (!server.waitForNewConnection(100, &timed_out)) {
            if (!timed_out) {
                throw std::runtime_error{
                    "browser authorization listener failed: " +
                    server.errorString().toStdString()
                };
            }
            continue;
        }
        std::unique_ptr<QTcpSocket> socket{server.nextPendingConnection()};
        if (!socket) {
            continue;
        }
        socket->setReadBufferSize(8192);
        QByteArray request;
        QElapsedTimer request_timer;
        request_timer.start();
        while (!request.contains("\r\n\r\n") && request.size() < 8192 &&
               request_timer.elapsed() < 2000 && !token.stop_requested() &&
               (socket->bytesAvailable() > 0 ||
                socket->state() == QAbstractSocket::ConnectedState)) {
            if (socket->bytesAvailable() == 0) {
                socket->waitForReadyRead(100);
            }
            request += socket->read(8192 - request.size());
        }
        const auto first_line =
            request.left(request.indexOf("\r\n")).split(' ');
        const auto target =
            first_line.size() == 3
                ? QUrl::fromEncoded(first_line[1], QUrl::StrictMode)
                : QUrl{};
        int host_count = 0;
        bool host_matches = false;
        for (const auto& header : request.split('\n')) {
            if (header.toLower().startsWith("host:")) {
                ++host_count;
                host_matches = header.mid(5).trimmed().compare(
                                   QString{"localhost:%1"}
                                       .arg(server.serverPort())
                                       .toLatin1(),
                                   Qt::CaseInsensitive
                               ) == 0;
            }
        }
        const bool valid_http =
            request.size() < 8192 && request.contains("\r\n\r\n") &&
            host_count == 1 && host_matches && first_line.size() == 3 &&
            first_line[2] == "HTTP/1.1" && first_line[0] == "GET" &&
            target.isValid() && target.isRelative() &&
            target.authority().isEmpty() && !target.hasFragment() &&
            first_line[1].startsWith('/');
        auth::AuthResult<void> callback = std::unexpected(
            auth::AuthError{
                .code = auth::AuthErrorCode::invalid_response,
                .message = "invalid authorization callback HTTP request",
            }
        );
        if (valid_http) {
            callback = session.accept_callback(
                "http://localhost:" + std::to_string(server.serverPort()) +
                    first_line[1].toStdString(),
                token
            );
        }
        const bool valid = callback.has_value() ||
                           session.state() == auth::AuthCodeState::failed;
        const QByteArray body = valid ? "Authorization received. Return to "
                                        "OneDrive C++. You may close this tab."
                                      : "Invalid authorization callback.";
        const QByteArray reply =
            (valid ? QByteArray{"HTTP/1.1 200 OK\r\n"}
                   : QByteArray{"HTTP/1.1 400 Bad Request\r\n"}) +
            "Content-Type: text/plain; charset=utf-8\r\nCache-Control: "
            "no-store\r\nConnection: close\r\nContent-Length: " +
            QByteArray::number(body.size()) + "\r\n\r\n" + body;
        socket->write(reply);
        socket->waitForBytesWritten(100);
        socket->disconnectFromHost();
        if (callback ||
            session.state() != auth::AuthCodeState::awaiting_callback) {
            return callback;
        }
    }
}

LoginController::LoginController(
    AuthenticationFunction authenticate, QObject* parent
)
    : QObject{parent},
      authenticate_{std::move(authenticate)} {
    if (!authenticate_) {
        authenticate_ =
            [](const config::Config& config,
               const std::function<void(const QString&)>& open_browser,
               std::stop_token token) {
                const app::ProductionRuntimeFactory production;
                const app::RuntimeFactory factory{
                    util::borrowed_proxy, production
                };
                return app::authenticate_auth_code_account(
                    config,
                    factory,
                    [&](auth::AuthCodeSession& session, std::stop_token stop) {
                        return request_browser_authorization(
                            session, open_browser, stop
                        );
                    },
                    token
                );
            };
    }
}

LoginController::~LoginController() {
    cancel();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool LoginController::running() const noexcept {
    return running_;
}

void LoginController::cancel() {
    worker_.request_stop();
}

void LoginController::start(const std::filesystem::path& config_file) {
    if (running_) {
        throw std::logic_error{"login is already running"};
    }
    running_ = true;
    try {
        worker_ = std::jthread{[this, config_file](std::stop_token token) {
            QString error;
            bool cancelled = false;
            try {
                const auto config = config::Config::load(config_file);
                config::validate_sync_state_directories(
                    config.sync_data_directory, config.state_directory
                );
                const auto result = authenticate_(
                    config,
                    [this, token](const QString& url) {
                        if (!token.stop_requested()) {
                            Q_EMIT browser_requested(url);
                        }
                    },
                    token
                );
                if (!result) {
                    cancelled =
                        result.error().code == auth::AuthErrorCode::cancelled;
                    error = QString::fromStdString(result.error().message);
                }
            } catch (const std::exception& exception) {
                error = QString::fromUtf8(exception.what());
            }
            QMetaObject::invokeMethod(
                this,
                [this, error = std::move(error), cancelled] {
                    worker_.join();
                    running_ = false;
                    Q_EMIT finished(error, cancelled);
                },
                Qt::QueuedConnection
            );
        }};
    } catch (...) {
        running_ = false;
        throw;
    }
}

} // namespace onedrive::gui
