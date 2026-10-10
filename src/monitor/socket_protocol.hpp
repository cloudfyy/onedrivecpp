#pragma once

#include "monitor/socket.hpp"

#include <stdexcept>
#include <utility>

namespace onedrive::monitor::detail {

enum class SocketPhase {
    connecting,
    websocket_connected,
    engine_open,
    ready,
    listening,
    disconnected,
    stopped,
};
enum class ProtocolEffect {
    send,
    connected,
    notification,
    disconnected,
    close,
};
struct ProtocolCommand {
    ProtocolEffect effect;
    std::string text;
    bool operator==(const ProtocolCommand&) const = default;
};
enum class WebSocketControl { ping, pong, close, binary };

// This is the wire protocol only; channel leases and retry belong to
// notify.hpp.
class SocketProtocol {
public:
    using Time = std::chrono::milliseconds;
    static constexpr std::size_t message_limit = std::size_t{1024} * 1024;

    [[nodiscard]] SocketPhase phase() const noexcept {
        return phase_;
    }
    std::vector<ProtocolCommand> connected() {
        if (phase_ != SocketPhase::connecting)
            return fail("unexpected WebSocket connection event");
        phase_ = SocketPhase::websocket_connected;
        return {};
    }
    // final is true only on the last chunk of the last WebSocket message frame.
    std::vector<ProtocolCommand>
    text(std::string_view chunk, bool final, Time now) {
        if (terminal())
            return {};
        if (phase_ == SocketPhase::connecting)
            return fail("Socket.IO data preceded the WebSocket connection");
        if (chunk.size() > message_limit - pending_.size())
            return fail("Socket.IO message exceeded its size limit");
        pending_.append(chunk);
        if (!final)
            return {};
        auto message = std::exchange(pending_, {});
        return frame(message, now);
    }
    std::vector<ProtocolCommand> control(WebSocketControl control) {
        if (terminal())
            return {};
        switch (control) {
        case WebSocketControl::ping:
        case WebSocketControl::pong:
            // libcurl automatically answers WebSocket control pings.
            return {};
        case WebSocketControl::close:
            return fail("Socket.IO peer closed the connection");
        case WebSocketControl::binary:
            return fail("unexpected binary Socket.IO frame");
        }
        return {};
    }
    std::vector<ProtocolCommand> tick(Time now) {
        if (active() && now - last_ping_ > heartbeat_timeout_)
            return fail("Socket.IO heartbeat timed out");
        return {};
    }
    std::vector<ProtocolCommand> fail(std::string reason) {
        if (terminal())
            return {};
        phase_ = SocketPhase::disconnected;
        pending_.clear();
        return {
            {ProtocolEffect::close, std::move(reason)},
            {ProtocolEffect::disconnected, {}},
        };
    }
    std::vector<ProtocolCommand> stop() {
        if (phase_ == SocketPhase::stopped)
            return {};
        phase_ = SocketPhase::stopped;
        pending_.clear();
        return {{ProtocolEffect::close, {}}};
    }

private:
    [[nodiscard]] bool terminal() const noexcept {
        return phase_ == SocketPhase::disconnected ||
               phase_ == SocketPhase::stopped;
    }
    [[nodiscard]] bool active() const noexcept {
        return phase_ == SocketPhase::engine_open ||
               phase_ == SocketPhase::ready || phase_ == SocketPhase::listening;
    }
    std::vector<ProtocolCommand> frame(std::string_view message, Time now) {
        if (message.starts_with('0')) {
            const auto timeout = socket_io_heartbeat_timeout(message);
            if (!timeout)
                return fail("invalid Socket.IO open frame");
            if (active())
                return {};
            phase_ = SocketPhase::engine_open;
            heartbeat_timeout_ = *timeout;
            last_ping_ = now;
            // Preserve the transport's connected publication at Engine.IO open.
            return {
                {ProtocolEffect::send, "40"},
                {ProtocolEffect::send, "40/notifications"},
                {ProtocolEffect::connected, {}},
            };
        }
        if (message.starts_with('2')) {
            last_ping_ = now;
            return {{ProtocolEffect::send, "3"}};
        }
        if (message.starts_with("41"))
            return fail("Socket.IO namespace disconnected");
        if (message.starts_with("44"))
            return fail("Socket.IO namespace rejected the connection");
        if (message == "1")
            return fail("Engine.IO peer closed the connection");
        if (message.starts_with("40")) {
            if (!active())
                return fail("Socket.IO readiness preceded Engine.IO open");
            if (message == "40" || message.starts_with("40{")) {
                if (phase_ != SocketPhase::listening)
                    phase_ = SocketPhase::ready;
            } else if (message == "40/notifications" ||
                       message.starts_with("40/notifications,")) {
                phase_ = SocketPhase::listening;
            }
            return {};
        }
        if (active() && socket_io_notification(message))
            return {{ProtocolEffect::notification, {}}};
        // Other Socket.IO events/acks and Engine.IO pong/noop are not wakeups.
        if (message.starts_with("42") || message.starts_with("43") ||
            message == "3" || message == "6")
            return {};
        return fail("unexpected Socket.IO frame");
    }

    SocketPhase phase_{SocketPhase::connecting};
    Time heartbeat_timeout_{};
    Time last_ping_{};
    std::string pending_;
};

} // namespace onedrive::monitor::detail
