#include "main_window.hpp"
#include "configuration.hpp"
#include "onedrive/app/factory.hpp"
#include "onedrive/app/queries.hpp"
#include <QDesktopServices>
#include <QLabel>
#include <QPushButton>
#include <QUrl>
#include <exception>

namespace onedrive::gui {

void MainWindow::connect_login_controller() {
    connect(
        login_button_, &QPushButton::clicked, this, &MainWindow::start_login
    );
    connect(account_action_button_, &QPushButton::clicked, this, [this] {
        if (login_.running()) {
            cancel_login();
        } else {
            logout();
        }
    });
    connect(
        &login_,
        &LoginController::browser_requested,
        this,
        [this](const QString& url) {
            if (cancelling_ || closing_) {
                return;
            }
            login_url_->setText(
                tr("Complete authorization in your system browser.")
            );
            login_url_is_instruction_ = true;
            authorization_url_ = url;
            set_login_status(QT_TR_NOOP(
                "Waiting for browser authorization (up to 5 minutes)..."
            ));
            open_login_url();
        },
        Qt::QueuedConnection
    );
    connect(
        &login_,
        &LoginController::finished,
        this,
        [this](const QString& error, bool cancelled) {
            login_url_->clear();
            login_url_is_instruction_ = false;
            authorization_url_.clear();
            refresh_actions();
            settings_->setEnabled(true);
            if (cancelled) {
                set_login_status(QT_TR_NOOP("Login cancelled."));
            } else if (!error.isEmpty()) {
                set_login_status(
                    QT_TR_NOOP("Login failed: %1"),
                    error,
                    events::MessageKind::error
                );
            } else if (closing_) {
                set_login_status(
                    QT_TR_NOOP("Signed in. Account credentials saved."),
                    {},
                    events::MessageKind::information
                );
            } else {
                try {
                    state_ = load_app_state_view_model(state_.config_file);
                    refresh_settings(true);
                    if (state_.can_sign_in()) {
                        set_login_status(
                            QT_TR_NOOP("Signed in. Account credentials saved."),
                            {},
                            events::MessageKind::information
                        );
                    } else {
                        const auto& detail = !state_.configuration_error.empty()
                                                 ? state_.configuration_error
                                             : !state_.account_error.empty()
                                                 ? state_.account_error
                                                 : state_.authentication_error;
                        set_login_status(
                            QT_TR_NOOP(
                                "Login completed, but account status could not "
                                "be "
                                "refreshed: %1"
                            ),
                            QString::fromStdString(detail),
                            events::MessageKind::error
                        );
                    }
                } catch (const std::exception& exception) {
                    set_login_status(
                        QT_TR_NOOP(
                            "Login completed, but account status could not be "
                            "refreshed: %1"
                        ),
                        QString::fromUtf8(exception.what()),
                        events::MessageKind::error
                    );
                }
            }
            if (closing_) {
                close();
            }
        }
    );
}

void MainWindow::set_login_status(
    const char* source,
    const QString& detail,
    events::MessageKind kind,
    bool record
) {
    login_status_source_ = source;
    login_status_detail_ = detail;
    auto text = tr(source);
    if (!detail.isNull()) {
        text = text.arg(detail);
    }
    login_status_->setText(text);
    if (record) {
        log_message(LogOperation::account, kind, source, detail);
    }
}

void MainWindow::start_login() {
    if (!state_.can_sign_in() ||
        state_.account_status == AccountStatus::credentials_available ||
        login_.running() || synchronization_.running() || monitor_busy()) {
        return;
    }
    cancelling_ = false;
    settings_->setEnabled(false);
    login_button_->setEnabled(false);
    set_login_status(QT_TR_NOOP("Starting browser login..."));
    try {
        synchronization_.invalidate_preview();
        login_.start(state_.config_file);
        refresh_actions();
    } catch (const std::exception& error) {
        settings_->setEnabled(true);
        refresh_actions();
        set_login_status(
            QT_TR_NOOP("Cannot start login: %1"),
            QString::fromUtf8(error.what()),
            events::MessageKind::error
        );
    }
}

void MainWindow::cancel_login() {
    cancelling_ = true;
    login_url_->clear();
    login_url_is_instruction_ = false;
    authorization_url_.clear();
    refresh_actions();
    set_login_status(QT_TR_NOOP("Cancelling login..."));
    login_.cancel();
}

void MainWindow::logout() {
    if (login_.running() || synchronization_.running() || monitor_busy() ||
        state_.account_status != AccountStatus::credentials_available) {
        return;
    }
    try {
        bool missing = false;
        const auto config = load_configuration(state_.config_file, missing);
        const app::ProductionRuntimeFactory production;
        const app::RuntimeFactory factory{util::borrowed_proxy, production};
        const bool removed = app::logout_account(config, factory);
        synchronization_.invalidate_preview();
        state_ = load_app_state_view_model(state_.config_file);
        refresh_settings(true);
        set_login_status(
            removed ? QT_TR_NOOP("Signed out. Saved credentials removed.")
                    : QT_TR_NOOP("No saved authentication was present."),
            {},
            events::MessageKind::information
        );
    } catch (const std::exception& error) {
        set_login_status(
            QT_TR_NOOP("Cannot log out: %1"),
            QString::fromUtf8(error.what()),
            events::MessageKind::error
        );
    }
}

void MainWindow::open_login_url() {
    const QUrl url{authorization_url_, QUrl::StrictMode};
    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() ||
        !url.userInfo().isEmpty()) {
        set_login_status(
            QT_TR_NOOP(
                "Cannot open browser: the login URL is not a valid HTTPS "
                "address."
            ),
            {},
            events::MessageKind::error
        );
        login_.cancel();
        return;
    }
    if (!QDesktopServices::openUrl(url)) {
        set_login_status(
            QT_TR_NOOP(
                "Cannot open the system browser. Copy the login URL below into "
                "your browser."
            ),
            {},
            events::MessageKind::warning
        );
        login_url_->setText(authorization_url_);
        login_url_is_instruction_ = false;
    }
}

} // namespace onedrive::gui
