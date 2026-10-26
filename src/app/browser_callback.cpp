#include "app/browser_callback.hpp"

#include "onedrive/util/system_error.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace onedrive::app::detail {
namespace {

constexpr int authorization_poll_interval_milliseconds = 100;
constexpr int reply_write_poll_interval_milliseconds = 10;

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

auth::AuthResult<void> run_loopback_browser_authorization(
    auth::AuthCodeSession& session,
    const std::function<BrowserOpenAction(std::string_view)>& on_authorize_url,
    const BrowserCallbackMessages& messages,
    bool allow_bind_fallback,
    bool& bind_or_open_aborted,
    const std::stop_token& token
) {
    bind_or_open_aborted = false;
    if (token.stop_requested()) {
        const auto cancelled = session.begin("http://localhost/", token);
        return std::unexpected(cancelled.error());
    }
    auto server = bind_loopback_server();
    if (!server) {
        if (allow_bind_fallback) {
            bind_or_open_aborted = true;
            return std::unexpected(
                auth::AuthError{
                    .code = auth::AuthErrorCode::cancelled,
                    .message = "cannot bind local browser callback port",
                }
            );
        }
        util::throw_errno_error(messages.listen_error_prefix);
    }
    const auto redirect =
        "http://localhost:" + std::to_string(server->port) + "/";
    const auto url = session.begin(redirect, token);
    if (!url) {
        return std::unexpected(url.error());
    }
    if (on_authorize_url(*url) == BrowserOpenAction::abort_before_callback) {
        bind_or_open_aborted = true;
        return std::unexpected(
            auth::AuthError{
                .code = auth::AuthErrorCode::cancelled,
                .message = "cannot launch system browser",
            }
        );
    }

    BrowserRequest request{
        messages.received_message, messages.invalid_message
    };
    const auto started = std::chrono::steady_clock::now();
    const auto now = [&] {
        return std::chrono::duration_cast<BrowserRequest::Time>(
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
            util::throw_errno_error(messages.listener_failed_prefix);
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
            util::throw_errno_error(messages.listener_failed_prefix);
        }

        auth::AuthResult<void> callback = std::unexpected(
            auth::AuthError{
                .code = auth::AuthErrorCode::invalid_response,
                .message = messages.invalid_request_error,
            }
        );
        auto commands = request.connected(now());
        while (!commands.empty()) {
            std::vector<BrowserCommand> next;
            for (const auto command : commands) {
                switch (command) {
                case BrowserCommand::read: {
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
                        BrowserRequest::request_limit -
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
                case BrowserCommand::validate: {
                    const auto target = browser_request_target(
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
                        callback ? CallbackOutcome::accepted
                        : session.state() == auth::AuthCodeState::failed
                            ? CallbackOutcome::rejected
                        : session.state() !=
                                auth::AuthCodeState::awaiting_callback
                            ? CallbackOutcome::interrupted
                            : CallbackOutcome::invalid;
                    next = request.validated(outcome, now());
                    break;
                }
                case BrowserCommand::write: {
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
                case BrowserCommand::flush:
                    next = request.flushed(0, now());
                    break;
                case BrowserCommand::close_connection:
                    if (client) {
                        static_cast<void>(::shutdown(client.get(), SHUT_WR));
                        client.reset();
                    }
                    break;
                case BrowserCommand::listen:
                    break;
                case BrowserCommand::finish:
                    return callback;
                }
            }
            commands = std::move(next);
        }
    }
}

} // namespace onedrive::app::detail
