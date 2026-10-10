#include "login_controller.hpp"
#include "browser_request.hpp"

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
    detail::BrowserRequest request;
    QElapsedTimer timer;
    timer.start();
    const auto now = [&] {
        return detail::BrowserRequest::Time{timer.elapsed()};
    };
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
        auth::AuthResult<void> callback = std::unexpected(
            auth::AuthError{
                .code = auth::AuthErrorCode::invalid_response,
                .message = "invalid authorization callback HTTP request",
            }
        );
        auto commands = request.connected(now());
        while (!commands.empty()) {
            auto next = std::vector<detail::BrowserCommand>{};
            for (const auto command : commands) {
                using Command = detail::BrowserCommand;
                switch (command) {
                case Command::read: {
                    if (const auto ready = session.check(token); !ready) {
                        callback = ready;
                        next = request.cancel();
                        break;
                    }
                    if (socket->bytesAvailable() == 0)
                        socket->waitForReadyRead(100);
                    const auto bytes = socket->read(
                        static_cast<qint64>(
                            detail::BrowserRequest::request_limit -
                            request.request().size()
                        )
                    );
                    if (!bytes.isEmpty())
                        next = request.bytes(
                            std::string_view{
                                bytes.constData(),
                                static_cast<std::size_t>(bytes.size())
                            },
                            now()
                        );
                    else if (socket->state() != QAbstractSocket::ConnectedState)
                        next = request.peer_closed(now());
                    else
                        next = request.tick(now());
                    break;
                }
                case Command::validate: {
                    const auto target = detail::browser_request_target(
                        request.request(),
                        "localhost:" + std::to_string(server.serverPort())
                    );
                    if (target) {
                        const auto encoded =
                            QByteArray::fromStdString(std::string{*target});
                        const auto parsed =
                            QUrl::fromEncoded(encoded, QUrl::StrictMode);
                        if (parsed.isValid() && parsed.isRelative() &&
                            parsed.authority().isEmpty() &&
                            !parsed.hasFragment()) {
                            callback = session.accept_callback(
                                "http://localhost:" +
                                    std::to_string(server.serverPort()) +
                                    std::string{*target},
                                token
                            );
                        }
                    }
                    const auto outcome =
                        callback ? detail::CallbackOutcome::accepted
                        : session.state() == auth::AuthCodeState::failed
                            ? detail::CallbackOutcome::rejected
                        : session.state() !=
                                auth::AuthCodeState::awaiting_callback
                            ? detail::CallbackOutcome::interrupted
                            : detail::CallbackOutcome::invalid;
                    next = request.validated(outcome, now());
                    break;
                }
                case Command::write: {
                    const auto remaining = request.remaining_reply();
                    const auto accepted = socket->write(
                        remaining.data(), static_cast<qint64>(remaining.size())
                    );
                    if (accepted < 0) {
                        next = request.peer_closed(now());
                    } else {
                        if (accepted == 0)
                            socket->waitForBytesWritten(10);
                        next = request.accepted(
                            static_cast<std::size_t>(accepted), now()
                        );
                    }
                    break;
                }
                case Command::flush:
                    if (socket->bytesToWrite() > 0)
                        socket->waitForBytesWritten(10);
                    next = request.flushed(
                        static_cast<std::size_t>(socket->bytesToWrite()), now()
                    );
                    break;
                case Command::close_connection:
                    socket->disconnectFromHost();
                    break;
                case Command::listen:
                    break;
                case Command::finish:
                    return callback;
                }
            }
            commands = std::move(next);
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
