#include "browser_auth.hpp"

#include "app/browser_callback.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <optional>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace onedrive::app::detail {
namespace {

constexpr int browser_spawn_check_attempts = 15;
constexpr auto browser_spawn_check_interval = std::chrono::milliseconds{10};

struct OverrideState {
    bool active{false};
    bool gui_available{false};
    BrowserLauncher launcher;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local OverrideState browser_override{};

const std::streambuf* initial_cout_streambuf() noexcept {
    static const std::streambuf* const buffer = std::cout.rdbuf();
    return buffer;
}

const int force_initial_cout_init = []() noexcept {
    static_cast<void>(initial_cout_streambuf());
    return 0;
}();

bool non_empty_env(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0';
}

std::optional<std::filesystem::path> find_xdg_open() noexcept {
    try {
        if (const char* path_env = std::getenv("PATH");
            path_env != nullptr && *path_env != '\0') {
            std::string_view remaining{path_env};
            while (!remaining.empty()) {
                const auto delimiter = remaining.find(':');
                const auto entry = remaining.substr(0, delimiter);
                if (!entry.empty()) {
                    auto candidate =
                        std::filesystem::path{entry} / "xdg-open";
                    if (::access(candidate.c_str(), X_OK) == 0) {
                        return candidate;
                    }
                }
                if (delimiter == std::string_view::npos) {
                    break;
                }
                remaining.remove_prefix(delimiter + 1);
            }
        }
        std::filesystem::path fallback{"/usr/bin/xdg-open"};
        if (::access(fallback.c_str(), X_OK) == 0) {
            return fallback;
        }
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

class ChildProcessReaper final {
public:
    ChildProcessReaper() = default;
    explicit ChildProcessReaper(pid_t pid) noexcept : pid_{pid} {
    }
    ~ChildProcessReaper() {
        if (pid_ > 0) {
            int status = 0;
            static_cast<void>(::waitpid(pid_, &status, WNOHANG));
        }
    }

    ChildProcessReaper(const ChildProcessReaper&) = delete;
    ChildProcessReaper& operator=(const ChildProcessReaper&) = delete;
    ChildProcessReaper(ChildProcessReaper&& other) noexcept
        : pid_{std::exchange(other.pid_, -1)} {
    }
    ChildProcessReaper& operator=(ChildProcessReaper&& other) noexcept {
        if (this != &other) {
            if (pid_ > 0) {
                int status = 0;
                static_cast<void>(::waitpid(pid_, &status, WNOHANG));
            }
            pid_ = std::exchange(other.pid_, -1);
        }
        return *this;
    }

private:
    pid_t pid_{-1};
};

bool launch_system_browser(
    const std::string& url, ChildProcessReaper& reaper
) noexcept {
    static_cast<void>(force_initial_cout_init);
    if (browser_override.active) {
        if (!browser_override.launcher) {
            return false;
        }
        try {
            return browser_override.launcher(url);
        } catch (...) {
            return false;
        }
    }
    const auto executable = find_xdg_open();
    if (!executable) {
        return false;
    }
    posix_spawn_file_actions_t actions{};
    if (::posix_spawn_file_actions_init(&actions) != 0) {
        return false;
    }
    struct ActionsGuard {
        posix_spawn_file_actions_t& value;
        explicit ActionsGuard(posix_spawn_file_actions_t& actions_ref) noexcept
            : value{actions_ref} {
        }
        ~ActionsGuard() {
            static_cast<void>(::posix_spawn_file_actions_destroy(&value));
        }
        ActionsGuard(const ActionsGuard&) = delete;
        ActionsGuard& operator=(const ActionsGuard&) = delete;
        ActionsGuard(ActionsGuard&&) = delete;
        ActionsGuard& operator=(ActionsGuard&&) = delete;
    } const guard{actions};
    if (::posix_spawn_file_actions_addopen(
            &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0
        ) != 0 ||
        ::posix_spawn_file_actions_addopen(
            &actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0
        ) != 0 ||
        ::posix_spawn_file_actions_addopen(
            &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0
        ) != 0) {
        return false;
    }
    std::string program = executable->string();
    std::string target = url;
    std::array<char*, 3> argv{program.data(), target.data(), nullptr};
    pid_t pid = -1;
    if (::posix_spawn(
            &pid, program.c_str(), &actions, nullptr, argv.data(), ::environ
        ) != 0 ||
        pid <= 0) {
        return false;
    }
    for (int attempt = 0; attempt < browser_spawn_check_attempts; ++attempt) {
        int status = 0;
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (waited < 0 && errno != EINTR) {
            return false;
        }
        std::this_thread::sleep_for(browser_spawn_check_interval);
    }
    reaper = ChildProcessReaper{pid};
    return true;
}

} // namespace

ScopedBrowserAuthOverride::ScopedBrowserAuthOverride(
    bool gui_available, BrowserLauncher launcher
)
    : previous_active_{browser_override.active},
      previous_gui_available_{browser_override.gui_available},
      previous_launcher_{std::move(browser_override.launcher)} {
    browser_override.active = true;
    browser_override.gui_available = gui_available;
    browser_override.launcher = std::move(launcher);
}

ScopedBrowserAuthOverride::~ScopedBrowserAuthOverride() {
    browser_override.active = previous_active_;
    browser_override.gui_available = previous_gui_available_;
    browser_override.launcher = std::move(previous_launcher_);
}

bool is_gui_auth_available() noexcept {
    if (browser_override.active) {
        return browser_override.gui_available;
    }
    if (std::cout.rdbuf() != initial_cout_streambuf() ||
        ::isatty(STDIN_FILENO) == 0 || ::isatty(STDOUT_FILENO) == 0) {
        return false;
    }
    if (!non_empty_env("DISPLAY") && !non_empty_env("WAYLAND_DISPLAY")) {
        return false;
    }
    return find_xdg_open().has_value();
}

auth::AuthResult<void> request_cli_browser_authorization(
    auth::AuthCodeSession& session,
    const cli::Console& console,
    bool fallback_allowed,
    bool& launch_failed,
    const std::stop_token& token
) {
    ChildProcessReaper browser_reaper;
    const BrowserCallbackMessages messages{};
    return run_loopback_browser_authorization(
        session,
        [&](std::string_view url) {
            const std::string target_url{url};
            const bool launched =
                launch_system_browser(target_url, browser_reaper);
            if (!launched && fallback_allowed) {
                return BrowserOpenAction::abort_before_callback;
            }
            if (!launched) {
                spdlog::warn(
                    "Could not launch system browser automatically; open the "
                    "authorization URL manually"
                );
            }
            console.message(
                cli::MessageKind::information,
                "browser_authorization",
                launched ? "Opened your default browser for Microsoft "
                           "sign-in. If it does not open, visit " +
                               target_url
                         : "Open " + target_url + " in your browser to sign in."
            );
            console.message(
                cli::MessageKind::information,
                "authorization_wait",
                "Waiting for browser authorization..."
            );
            return BrowserOpenAction::wait_for_callback;
        },
        messages,
        fallback_allowed,
        launch_failed,
        token
    );
}

} // namespace onedrive::app::detail
