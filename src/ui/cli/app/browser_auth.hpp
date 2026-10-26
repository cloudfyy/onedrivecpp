#pragma once

#include "onedrive/auth/auth_code.hpp"
#include "onedrive/ui/cli/console.hpp"

#include <functional>
#include <stop_token>
#include <string>

namespace onedrive::app::detail {

using BrowserLauncher = std::function<bool(const std::string& url)>;

class ScopedBrowserAuthOverride final {
public:
    ScopedBrowserAuthOverride(bool gui_available, BrowserLauncher launcher);
    ~ScopedBrowserAuthOverride();

    ScopedBrowserAuthOverride(const ScopedBrowserAuthOverride&) = delete;
    ScopedBrowserAuthOverride&
    operator=(const ScopedBrowserAuthOverride&) = delete;
    ScopedBrowserAuthOverride(ScopedBrowserAuthOverride&&) = delete;
    ScopedBrowserAuthOverride& operator=(ScopedBrowserAuthOverride&&) = delete;

private:
    bool previous_active_{false};
    bool previous_gui_available_{false};
    BrowserLauncher previous_launcher_;
};

[[nodiscard]] bool is_gui_auth_available() noexcept;

[[nodiscard]] auth::AuthResult<void> request_cli_browser_authorization(
    auth::AuthCodeSession& session,
    const cli::Console& console,
    bool fallback_allowed,
    bool& launch_failed,
    const std::stop_token& token = {}
);

} // namespace onedrive::app::detail
