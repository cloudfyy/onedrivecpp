#pragma once

#include "onedrive/app/authentication.hpp"
#include "onedrive/config/config.hpp"

#include <QObject>
#include <QString>

#include <functional>
#include <thread>

namespace onedrive::gui {

[[nodiscard]] auth::AuthResult<void> request_browser_authorization(
    auth::AuthCodeSession& session,
    const std::function<void(const QString&)>& open_browser,
    std::stop_token token = {}
);

using AuthenticationFunction =
    std::function<auth::AuthResult<app::AuthenticationResult>(
        const config::Config&,
        const std::function<void(const QString&)>&,
        std::stop_token
    )>;

class LoginController final : public QObject {
    Q_OBJECT

public:
    explicit LoginController(
        AuthenticationFunction authenticate = {}, QObject* parent = nullptr
    );
    ~LoginController() override;
    void start(const std::filesystem::path& config_file);
    void cancel();
    [[nodiscard]] bool running() const noexcept;

Q_SIGNALS:
    void browser_requested(QString url);
    void finished(QString error, bool cancelled);

private:
    AuthenticationFunction authenticate_;
    std::jthread worker_;
    bool running_{false};
};

} // namespace onedrive::gui
