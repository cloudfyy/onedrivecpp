#include "monitor/socket.hpp"
#include "monitor/socket_protocol.hpp"

#include "http/curl.hpp"
#include "http/proxy.hpp"
#include "onedrive/util/system_error.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"

#include <curl/curl.h>
#include <curl/websockets.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <sys/eventfd.h>
#include <thread>
#include <utility>
#include <unistd.h>

namespace onedrive::monitor::detail {
namespace {

using namespace std::chrono_literals;
using http::detail::throw_if_curl_error;

int stop_requested(
    void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t
) {
    const auto* stop_token = static_cast<const std::stop_token*>(context);
    return stop_token->stop_requested() ? 1 : 0;
}

void send_text(CURL* curl, std::string_view text) {
    std::size_t sent = 0;
    throw_if_curl_error(
        ::curl_ws_send(curl, text.data(), text.size(), &sent, 0, CURLWS_TEXT),
        "cannot send Socket.IO frame"
    );
    if (sent != text.size()) {
        throw std::runtime_error("incomplete Socket.IO frame send");
    }
}

struct ReceivedChunk {
    std::string text;
    int flags;
    bool final;
};

std::optional<ReceivedChunk> receive_chunk(CURL* curl) {
    std::array<char, std::size_t{16} * 1024> buffer{};
    std::size_t received = 0;
    const curl_ws_frame* frame = nullptr;
    const CURLcode result =
        ::curl_ws_recv(curl, buffer.data(), buffer.size(), &received, &frame);
    if (result == CURLE_AGAIN) {
        return std::nullopt;
    }
    throw_if_curl_error(result, "cannot receive Socket.IO frame");
    if (frame == nullptr) {
        throw std::runtime_error("Socket.IO frame metadata is missing");
    }
    return ReceivedChunk{
        std::string{buffer.data(), received},
        frame->flags,
        frame->bytesleft == 0 && (frame->flags & CURLWS_CONT) == 0,
    };
}

} // namespace

std::string socket_io_url(std::string_view notification_url) {
    const auto scheme = notification_url.find("://");
    if (scheme == std::string_view::npos ||
        notification_url.substr(0, scheme) != "https") {
        throw std::invalid_argument("notification URL must use HTTPS");
    }
    const auto authority_begin = scheme + 3;
    const auto path_begin = notification_url.find('/', authority_begin);
    const auto authority = notification_url.substr(
        authority_begin,
        path_begin == std::string_view::npos
            ? notification_url.size() - authority_begin
            : path_begin - authority_begin
    );
    if (authority.empty()) {
        throw std::invalid_argument("notification URL has no authority");
    }
    std::string_view query;
    if (path_begin != std::string_view::npos) {
        const auto query_begin = notification_url.find('?', path_begin);
        if (query_begin != std::string_view::npos) {
            query = notification_url.substr(query_begin + 1);
        }
    }
    std::string result = "wss://" + std::string{authority} +
                         "/socket.io/?EIO=4&transport=websocket";
    if (!query.empty()) {
        result += '&';
        result += query;
    }
    return result;
}

bool socket_io_notification(std::string_view frame) {
    if (!frame.starts_with("42")) {
        return false;
    }

    std::size_t payload = 2;
    if (payload < frame.size() && frame[payload] == '/') {
        payload = frame.find(',', payload);
        if (payload == std::string_view::npos) {
            return false;
        }
        ++payload;
    }
    if (payload >= frame.size() || frame[payload] != '[') {
        return false;
    }
    const auto value =
        nlohmann::json::parse(frame.substr(payload), nullptr, false);
    return value.is_array() && !value.empty() && value.front().is_string() &&
           value.front().get_ref<const std::string&>() == "notification";
}

std::optional<std::chrono::milliseconds>
socket_io_heartbeat_timeout(std::string_view frame) {
    if (!frame.starts_with("0{")) {
        return std::nullopt;
    }
    const auto value = nlohmann::json::parse(frame.substr(1), nullptr, false);
    if (!value.is_object()) {
        return std::nullopt;
    }
    const auto interval = value.find("pingInterval");
    const auto timeout = value.find("pingTimeout");
    if (interval == value.end() || timeout == value.end() ||
        !interval->is_number_unsigned() || !timeout->is_number_unsigned()) {
        return std::nullopt;
    }
    const auto interval_value = interval->get<std::uint64_t>();
    const auto timeout_value = timeout->get<std::uint64_t>();
    const auto maximum =
        static_cast<std::uint64_t>(std::chrono::milliseconds::max().count());
    if (interval_value == 0 || timeout_value == 0 || timeout_value > maximum ||
        interval_value > maximum - timeout_value) {
        return std::nullopt;
    }
    return std::chrono::milliseconds{interval_value + timeout_value};
}

class SocketIoTransport::Implementation final {
public:
    Implementation(
        http::ProxyOptions proxy, std::chrono::milliseconds connect_timeout
    )
        : proxy_{std::move(proxy)},
          connect_timeout_{connect_timeout},
          descriptor_{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)} {
        if (connect_timeout_ <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument(
                "WebSocket connect timeout must be positive"
            );
        }
        if (descriptor_.get() < 0) {
            throw std::system_error{
                errno,
                std::generic_category(),
                "cannot create notification event descriptor"
            };
        }
        if (proxy_.password_file) {
            proxy_password_ =
                http::detail::read_proxy_password(*proxy_.password_file);
        }
        if (proxy_.no_proxy) {
            no_proxy_ = http::detail::join_proxy_bypass_list(*proxy_.no_proxy);
        }
    }

    ~Implementation() {
        disconnect();
    }

    Implementation(const Implementation&) = delete;
    Implementation& operator=(const Implementation&) = delete;
    Implementation(Implementation&&) = delete;
    Implementation& operator=(Implementation&&) = delete;

    [[nodiscard]] int descriptor() const noexcept {
        return descriptor_.get();
    }

    void connect(std::string_view notification_url) {
        disconnect();
        worker_ = std::jthread{[this, url = socket_io_url(notification_url)](
                                   const std::stop_token& stop_token
                               ) { run(url, stop_token); }};
    }

    void disconnect() {
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
    }

    [[nodiscard]] std::vector<SocketEvent> drain() {
        std::uint64_t count = 0;
        while (::read(descriptor_.get(), &count, sizeof(count)) < 0 &&
               errno == EINTR) {
        }
        const std::scoped_lock lock{mutex_};
        return std::exchange(events_, {});
    }

private:
    void report_failure(std::string_view reason) noexcept {
        // A logger/error handler or event allocation must not terminate the
        // worker while it is already handling a transport failure.
        try {
            spdlog::warn("Notification WebSocket failed: {}", reason);
        } catch (...) {
            constexpr std::string_view message =
                "Cannot log notification WebSocket failure\n";
            static_cast<void>(
                ::write(STDERR_FILENO, message.data(), message.size())
            );
        }
        try {
            publish(SocketEvent::disconnected);
        } catch (...) {
            constexpr std::string_view message =
                "Cannot publish notification WebSocket disconnection\n";
            static_cast<void>(
                ::write(STDERR_FILENO, message.data(), message.size())
            );
        }
    }

    void publish(SocketEvent event) {
        {
            const std::scoped_lock lock{mutex_};
            events_.push_back(event);
        }
        constexpr std::uint64_t value = 1;
        if (::write(descriptor_.get(), &value, sizeof(value)) < 0 &&
            errno != EAGAIN) {
            spdlog::warn(
                "Cannot signal Monitor notification event: {}",
                onedrive::util::system_error_message(errno)
            );
        }
    }

    void configure(
        CURL* curl, const std::string& url, const std::stop_token& stop_token
    ) const {
        throw_if_curl_error(
            ::curl_easy_setopt(curl, CURLOPT_URL, url.c_str()),
            "cannot configure WebSocket URL"
        );
        throw_if_curl_error(
            ::curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L),
            "cannot enable WebSocket mode"
        );
        throw_if_curl_error(
            ::curl_easy_setopt(
                curl,
                CURLOPT_CONNECTTIMEOUT_MS,
                static_cast<long>(connect_timeout_.count())
            ),
            "cannot configure WebSocket connect timeout"
        );
        throw_if_curl_error(
            ::curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L),
            "cannot configure WebSocket signal handling"
        );
        throw_if_curl_error(
            ::curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L),
            "cannot enable WebSocket cancellation"
        );
        throw_if_curl_error(
            ::curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &stop_requested),
            "cannot configure WebSocket cancellation callback"
        );
        throw_if_curl_error(
            ::curl_easy_setopt(
                curl,
                CURLOPT_XFERINFODATA,
                static_cast<const void*>(&stop_token)
            ),
            "cannot configure WebSocket cancellation state"
        );
        if (proxy_.url) {
            throw_if_curl_error(
                ::curl_easy_setopt(curl, CURLOPT_PROXY, proxy_.url->c_str()),
                "cannot configure WebSocket proxy"
            );
        }
        if (no_proxy_) {
            throw_if_curl_error(
                ::curl_easy_setopt(curl, CURLOPT_NOPROXY, no_proxy_->c_str()),
                "cannot configure WebSocket proxy bypass"
            );
        }
        if (proxy_.username) {
            throw_if_curl_error(
                ::curl_easy_setopt(
                    curl, CURLOPT_PROXYUSERNAME, proxy_.username->c_str()
                ),
                "cannot configure WebSocket proxy username"
            );
        }
        if (proxy_password_) {
            throw_if_curl_error(
                ::curl_easy_setopt(
                    curl, CURLOPT_PROXYPASSWORD, proxy_password_->c_str()
                ),
                "cannot configure WebSocket proxy password"
            );
        }
        if (proxy_.url) {
            throw_if_curl_error(
                ::curl_easy_setopt(
                    curl,
                    CURLOPT_PROXYAUTH,
                    http::detail::curl_proxy_auth(proxy_.auth)
                ),
                "cannot configure WebSocket proxy authentication"
            );
        }
        if (proxy_.ca_file) {
            throw_if_curl_error(
                ::curl_easy_setopt(
                    curl, CURLOPT_PROXY_CAINFO, proxy_.ca_file->c_str()
                ),
                "cannot configure WebSocket proxy CA"
            );
        }
    }

    void
    run(const std::string& url, const std::stop_token& stop_token) noexcept {
        try {
            SocketProtocol protocol;
            CURL* connection = nullptr;
            const auto now = [] {
                return std::chrono::duration_cast<SocketProtocol::Time>(
                    std::chrono::steady_clock::now().time_since_epoch()
                );
            };
            const auto execute =
                [&](const std::vector<ProtocolCommand>& commands) {
                    for (const auto& command : commands) {
                        switch (command.effect) {
                        case ProtocolEffect::send:
                            send_text(connection, command.text);
                            break;
                        case ProtocolEffect::connected:
                            publish(SocketEvent::connected);
                            break;
                        case ProtocolEffect::notification:
                            publish(SocketEvent::notification);
                            break;
                        case ProtocolEffect::disconnected:
                            publish(SocketEvent::disconnected);
                            break;
                        case ProtocolEffect::close:
                            if (!command.text.empty())
                                spdlog::warn(
                                    "Notification WebSocket failed: {}",
                                    command.text
                                );
                            // The lease closes the transport when the loop
                            // exits.
                            break;
                        }
                    }
                };
            throw_if_curl_error(
                http::detail::initialize_curl(), "cannot initialize libcurl"
            );
            const http::detail::CurlHandleLease handle;
            if (!handle) {
                throw std::runtime_error("cannot create WebSocket handle");
            }
            configure(handle.get(), url, stop_token);
            throw_if_curl_error(
                ::curl_easy_perform(handle.get()),
                "cannot connect notification WebSocket"
            );
            connection = handle.get();
            if (stop_token.stop_requested()) {
                execute(protocol.stop());
                return;
            }
            execute(protocol.connected());
            curl_socket_t socket = CURL_SOCKET_BAD;
            throw_if_curl_error(
                ::curl_easy_getinfo(
                    handle.get(), CURLINFO_ACTIVESOCKET, &socket
                ),
                "cannot obtain notification WebSocket descriptor"
            );
            while (!stop_token.stop_requested()) {
                pollfd descriptor{
                    .fd = socket,
                    .events = POLLIN,
                    .revents = 0,
                };
                const int ready = ::poll(&descriptor, 1, 250);
                if (ready < 0 && errno == EINTR) {
                    continue;
                }
                if (ready < 0) {
                    throw std::system_error{
                        errno,
                        std::generic_category(),
                        "notification WebSocket poll failed"
                    };
                }
                if (ready == 0) {
                    execute(protocol.tick(now()));
                    if (protocol.phase() == SocketPhase::disconnected)
                        return;
                    continue;
                }
                if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) !=
                    0) {
                    throw std::runtime_error(
                        "notification WebSocket became unavailable"
                    );
                }
                while (!stop_token.stop_requested()) {
                    const auto chunk = receive_chunk(handle.get());
                    if (!chunk)
                        break;
                    if ((chunk->flags & CURLWS_CLOSE) != 0)
                        execute(protocol.control(WebSocketControl::close));
                    else if ((chunk->flags & CURLWS_PING) != 0)
                        execute(protocol.control(WebSocketControl::ping));
                    else if ((chunk->flags & CURLWS_PONG) != 0)
                        execute(protocol.control(WebSocketControl::pong));
                    else if ((chunk->flags & CURLWS_TEXT) != 0)
                        execute(
                            protocol.text(chunk->text, chunk->final, now())
                        );
                    else
                        execute(protocol.control(WebSocketControl::binary));
                    if (protocol.phase() == SocketPhase::disconnected)
                        return;
                }
                execute(protocol.tick(now()));
                if (protocol.phase() == SocketPhase::disconnected)
                    return;
            }
            execute(protocol.stop());
        } catch (const std::exception& error) {
            if (!stop_token.stop_requested()) {
                report_failure(error.what());
            }
        } catch (...) {
            if (!stop_token.stop_requested()) {
                report_failure("unknown error");
            }
        }
    }

    http::ProxyOptions proxy_;
    std::chrono::milliseconds connect_timeout_;
    std::optional<std::string> proxy_password_;
    std::optional<std::string> no_proxy_;
    onedrive::util::UniqueFD descriptor_;
    std::jthread worker_;
    std::mutex mutex_;
    std::vector<SocketEvent> events_;
};

SocketIoTransport::SocketIoTransport(
    http::ProxyOptions proxy, std::chrono::milliseconds connect_timeout
)
    : implementation_{
          std::make_unique<Implementation>(std::move(proxy), connect_timeout)
      } {
}

SocketIoTransport::~SocketIoTransport() = default;

int SocketIoTransport::descriptor() const noexcept {
    return implementation_->descriptor();
}

void SocketIoTransport::connect(std::string_view notification_url) {
    implementation_->connect(notification_url);
}

void SocketIoTransport::disconnect() {
    implementation_->disconnect();
}

std::vector<SocketEvent> SocketIoTransport::drain() {
    return implementation_->drain();
}

} // namespace onedrive::monitor::detail
