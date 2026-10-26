#include "browser_auth.hpp"

#include "onedrive/util/system_error.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"
#include "ui/gui/browser_request.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace onedrive::app::detail {
namespace {

constexpr int authorization_poll_interval_milliseconds = 100;
constexpr int reply_write_poll_interval_milliseconds = 10;
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

struct LoopbackServer {
    util::UniqueFD descriptor;
    std::uint16_t port{0};
};

std::optional<LoopbackServer> bind_loopback_server() noexcept {
    util::UniqueFD descriptor{
        ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)
    };
    if (!descriptor) {
        return std::nullopt;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(
            descriptor.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)
        ) != 0 ||
        ::listen(descriptor.get(), 4) != 0) {
        return std::nullopt;
    }
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(
            descriptor.get(),
            reinterpret_cast<sockaddr*>(&bound),
            &length
        ) != 0 ||
        length != sizeof(bound)) {
        return std::nullopt;
    }
    return LoopbackServer{
        .descriptor = std::move(descriptor),
        .port = ntohs(bound.sin_port),
    };
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
    launch_failed = false;
    if (token.stop_requested()) {
        const auto cancelled = session.begin("http://localhost/", token);
        return std::unexpected(cancelled.error());
    }
    auto server = bind_loopback_server();
    if (!server) {
        if (fallback_allowed) {
            launch_failed = true;
            return std::unexpected(
                auth::AuthError{
                    .code = auth::AuthErrorCode::cancelled,
                    .message = "cannot bind local browser callback port",
                }
            );
        }
        throw std::runtime_error{
            "Cannot listen for browser authorization on localhost"
        };
    }
    const auto redirect =
        "http://localhost:" + std::to_string(server->port) + "/";
    const auto url = session.begin(redirect, token);
    if (!url) {
        return std::unexpected(url.error());
    }

    ChildProcessReaper browser_reaper;
    const bool launched = launch_system_browser(*url, browser_reaper);
    if (!launched && fallback_allowed) {
        launch_failed = true;
        return std::unexpected(
            auth::AuthError{
                .code = auth::AuthErrorCode::cancelled,
                .message = "cannot launch system browser",
            }
        );
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
        launched
            ? "Opened your default browser for Microsoft sign-in. If it does "
              "not open, visit " +
                  *url
            : "Open " + *url + " in your browser to sign in."
    );
    console.message(
        cli::MessageKind::information,
        "authorization_wait",
        "Waiting for browser authorization..."
    );

    gui::detail::BrowserRequest request;
    const auto started = std::chrono::steady_clock::now();
    const auto now = [&] {
        return std::chrono::duration_cast<gui::detail::BrowserRequest::Time>(
            std::chrono::steady_clock::now() - started
        );
    };

    while (true) {
        if (const auto ready = session.check(token); !ready) {
            return ready;
        }
        pollfd listener_poll{
            .fd = server->descriptor.get(),
            .events = POLLIN,
            .revents = 0,
        };
        const int ready_count = ::poll(
            &listener_poll, 1, authorization_poll_interval_milliseconds
        );
        if (ready_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            util::throw_errno_error("Browser authorization listener failed");
        }
        if (ready_count == 0 || (listener_poll.revents & POLLIN) == 0) {
            continue;
        }
        util::UniqueFD client{::accept4(
            server->descriptor.get(),
            nullptr,
            nullptr,
            SOCK_NONBLOCK | SOCK_CLOEXEC
        )};
        if (!client) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            util::throw_errno_error(
                "Cannot accept browser authorization connection"
            );
        }

        auth::AuthResult<void> callback = std::unexpected(
            auth::AuthError{
                .code = auth::AuthErrorCode::invalid_response,
                .message = "Invalid authorization callback HTTP request.",
            }
        );
        auto commands = request.connected(now());
        while (!commands.empty()) {
            std::vector<gui::detail::BrowserCommand> next;
            for (const auto command : commands) {
                using Command = gui::detail::BrowserCommand;
                switch (command) {
                case Command::read: {
                    if (const auto ready = session.check(token); !ready) {
                        callback = ready;
                        next = request.cancel();
                        break;
                    }
                    pollfd client_poll{
                        .fd = client.get(),
                        .events = POLLIN,
                        .revents = 0,
                    };
                    const int poll_rc = ::poll(
                        &client_poll,
                        1,
                        authorization_poll_interval_milliseconds
                    );
                    if (poll_rc < 0 && errno != EINTR) {
                        next = request.peer_closed(now());
                        break;
                    }
                    std::array<char, 2048> buffer{};
                    const auto maximum_read = std::min(
                        buffer.size(),
                        gui::detail::BrowserRequest::request_limit -
                            request.request().size()
                    );
                    const ssize_t received =
                        ::recv(client.get(), buffer.data(), maximum_read, 0);
                    if (received > 0) {
                        next = request.bytes(
                            std::string_view{
                                buffer.data(),
                                static_cast<std::size_t>(received)
                            },
                            now()
                        );
                    } else if (received < 0 &&
                               (errno == EAGAIN || errno == EWOULDBLOCK ||
                                errno == EINTR)) {
                        next = request.tick(now());
                    } else {
                        next = request.peer_closed(now());
                    }
                    break;
                }
                case Command::validate: {
                    const auto target = gui::detail::browser_request_target(
                        request.request(),
                        "localhost:" + std::to_string(server->port)
                    );
                    if (target) {
                        callback = session.accept_callback(
                            "http://localhost:" +
                                std::to_string(server->port) +
                                std::string{*target},
                            token
                        );
                    }
                    const auto outcome =
                        callback ? gui::detail::CallbackOutcome::accepted
                        : session.state() == auth::AuthCodeState::failed
                            ? gui::detail::CallbackOutcome::rejected
                        : session.state() !=
                                auth::AuthCodeState::awaiting_callback
                            ? gui::detail::CallbackOutcome::interrupted
                            : gui::detail::CallbackOutcome::invalid;
                    next = request.validated(outcome, now());
                    break;
                }
                case Command::write: {
                    const auto remaining = request.remaining_reply();
                    const ssize_t sent = ::send(
                        client.get(),
                        remaining.data(),
                        remaining.size(),
                        MSG_NOSIGNAL
                    );
                    if (sent > 0) {
                        next = request.accepted(
                            static_cast<std::size_t>(sent), now()
                        );
                    } else if (sent == 0 ||
                               (sent < 0 &&
                                (errno == EAGAIN || errno == EWOULDBLOCK ||
                                 errno == EINTR))) {
                        pollfd write_poll{
                            .fd = client.get(),
                            .events = POLLOUT,
                            .revents = 0,
                        };
                        static_cast<void>(::poll(
                            &write_poll,
                            1,
                            reply_write_poll_interval_milliseconds
                        ));
                        next = request.tick(now());
                    } else {
                        next = request.peer_closed(now());
                    }
                    break;
                }
                case Command::flush:
                    next = request.flushed(0, now());
                    break;
                case Command::close_connection:
                    if (client) {
                        static_cast<void>(::shutdown(client.get(), SHUT_WR));
                        client.reset();
                    }
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

} // namespace onedrive::app::detail
