#pragma once

#include "onedrive/http/http_options.hpp"

#include <memory>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace onedrive::monitor::detail {

enum class SocketEvent {
    connected,
    notification,
    disconnected,
};

[[nodiscard]] std::string socket_io_url(std::string_view notification_url);
[[nodiscard]] bool socket_io_notification(std::string_view frame);
[[nodiscard]] std::optional<std::chrono::milliseconds>
socket_io_heartbeat_timeout(std::string_view frame);

class SocketIoTransport final {
public:
    SocketIoTransport(
        http::ProxyOptions proxy,
        std::chrono::milliseconds connect_timeout
    );
    ~SocketIoTransport();
    SocketIoTransport(const SocketIoTransport&) = delete;
    SocketIoTransport& operator=(const SocketIoTransport&) = delete;

    [[nodiscard]] int descriptor() const noexcept;
    void connect(std::string notification_url);
    void disconnect();
    [[nodiscard]] std::vector<SocketEvent> drain();

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace onedrive::monitor::detail
