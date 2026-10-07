#include "monitor/socket.hpp"

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

std::optional<std::string> receive_text(CURL* curl) {
    std::array<char, 16 * 1024> buffer{};
    std::string message;
    while (true) {
        std::size_t received = 0;
        const curl_ws_frame* frame = nullptr;
        const CURLcode result = ::curl_ws_recv(
            curl, buffer.data(), buffer.size(), &received, &frame
        );
        if (result == CURLE_AGAIN) {
            return message.empty() ? std::nullopt
                                   : std::optional{std::move(message)};
        }
        throw_if_curl_error(result, "cannot receive Socket.IO frame");
        if (frame == nullptr) {
            throw std::runtime_error("Socket.IO frame metadata is missing");
        }
        if ((frame->flags & CURLWS_CLOSE) != 0U) {
            throw std::runtime_error("Socket.IO peer closed the connection");
        }
        if ((frame->flags & (CURLWS_TEXT | CURLWS_CONT)) == 0U) {
            if ((frame->flags & CURLWS_PING) != 0U) {
                continue;
            }
            throw std::runtime_error("unexpected binary Socket.IO frame");
        }
        message.append(buffer.data(), received);
        if (frame->bytesleft == 0) {
            return message;
        }
    }
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
    if (interval_value == 0 || timeout_value == 0 ||
        interval_value >
            static_cast<std::uint64_t>(std::chrono::milliseconds::max().count()
            ) - timeout_value) {
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

    [[nodiscard]] int descriptor() const noexcept {
        return descriptor_.get();
    }

    void connect(std::string notification_url) {
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
            curl_socket_t socket = CURL_SOCKET_BAD;
            throw_if_curl_error(
                ::curl_easy_getinfo(
                    handle.get(), CURLINFO_ACTIVESOCKET, &socket
                ),
                "cannot obtain notification WebSocket descriptor"
            );
            bool engine_open = false;
            auto heartbeat_timeout = std::chrono::milliseconds::zero();
            auto last_ping = std::chrono::steady_clock::now();
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
                    if (engine_open &&
                        std::chrono::steady_clock::now() - last_ping >
                            heartbeat_timeout) {
                        throw std::runtime_error(
                            "Socket.IO heartbeat timed out"
                        );
                    }
                    continue;
                }
                if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) !=
                    0) {
                    throw std::runtime_error(
                        "notification WebSocket became unavailable"
                    );
                }
                const auto message = receive_text(handle.get());
                if (!message) {
                    continue;
                }
                if (message->starts_with('0')) {
                    const auto timeout = socket_io_heartbeat_timeout(*message);
                    if (!timeout) {
                        throw std::runtime_error(
                            "invalid Socket.IO open frame"
                        );
                    }
                    if (engine_open) {
                        continue;
                    }
                    engine_open = true;
                    heartbeat_timeout = *timeout;
                    last_ping = std::chrono::steady_clock::now();
                    send_text(handle.get(), "40");
                    send_text(handle.get(), "40/notifications");
                    publish(SocketEvent::connected);
                } else if (message->starts_with('2')) {
                    send_text(handle.get(), "3");
                    last_ping = std::chrono::steady_clock::now();
                } else if (message->starts_with("41")) {
                    throw std::runtime_error(
                        "Socket.IO namespace disconnected"
                    );
                } else if (engine_open && socket_io_notification(*message)) {
                    publish(SocketEvent::notification);
                }
            }
        } catch (const std::exception& error) {
            if (!stop_token.stop_requested()) {
                spdlog::warn("Notification WebSocket failed: {}", error.what());
                publish(SocketEvent::disconnected);
            }
        } catch (...) {
            if (!stop_token.stop_requested()) {
                spdlog::warn(
                    "Notification WebSocket failed with an unknown error"
                );
                publish(SocketEvent::disconnected);
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

void SocketIoTransport::connect(std::string notification_url) {
    implementation_->connect(std::move(notification_url));
}

void SocketIoTransport::disconnect() {
    implementation_->disconnect();
}

std::vector<SocketEvent> SocketIoTransport::drain() {
    return implementation_->drain();
}

} // namespace onedrive::monitor::detail
