#include "login_controller.hpp"
#include "browser_request.hpp"
#include "configuration.hpp"

#include "onedrive/app/factory.hpp"
#include "app/checks.hpp"

#include <QCoreApplication>
#include <QMetaObject>

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace onedrive::gui {

auth::AuthResult<void> request_browser_authorization(
    auth::AuthCodeSession& session,
    const std::function<void(const QString&)>& open_browser,
    const std::stop_token& token
) {
    if (!open_browser) {
        throw std::invalid_argument{
            QCoreApplication::translate(
                "BrowserLogin", "Browser login requires a browser callback."
            )
                .toStdString()
        };
    }
    const app::detail::BrowserCallbackMessages messages{
        .received_message =
            QCoreApplication::translate(
                "BrowserLogin",
                "Authorization received. Return to OneDrive client. "
                "You may close this tab."
            )
                .toStdString(),
        .invalid_message =
            QCoreApplication::translate(
                "BrowserLogin", "Invalid authorization callback."
            )
                .toStdString(),
        .invalid_request_error =
            QCoreApplication::translate(
                "BrowserLogin",
                "Invalid authorization callback HTTP request."
            )
                .toStdString(),
        .listen_error_prefix =
            QCoreApplication::translate(
                "BrowserLogin", "Cannot listen for browser authorization"
            )
                .toStdString(),
        .listener_failed_prefix =
            QCoreApplication::translate(
                "BrowserLogin", "Browser authorization listener failed"
            )
                .toStdString(),
    };
    bool bind_or_open_aborted = false;
    return app::detail::run_loopback_browser_authorization(
        session,
        [&](std::string_view url) {
            open_browser(QString::fromStdString(std::string{url}));
            return app::detail::BrowserOpenAction::wait_for_callback;
        },
        messages,
        false,
        bind_or_open_aborted,
        token
    );
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
               const std::stop_token& token) {
                const app::ProductionRuntimeFactory production;
                const app::RuntimeFactory factory{
                    util::borrowed_proxy, production
                };
                return app::authenticate_auth_code_account(
                    config,
                    factory,
                    [&](
                        auth::AuthCodeSession& session,
                        const std::stop_token& stop
                    ) {
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
        throw std::logic_error{tr("Login is already running.").toStdString()};
    }
    running_ = true;
    try {
        worker_ = std::jthread{[this, config_file](const std::stop_token& token) {
            QString error;
            bool cancelled = false;
            try {
                bool missing = false;
                const auto config = load_configuration(config_file, missing);
                app::detail::validate_authentication_config(config);
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
                    error = QString::fromStdString(
                        auth::describe_error(result.error())
                    );
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
