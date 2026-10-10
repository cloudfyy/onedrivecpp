#include "monitor/socket.hpp"
#include "monitor/socket_protocol.hpp"
#include "support/common.hpp"

#include <cstdlib>
#include <poll.h>
#include <stdexcept>

namespace {

using onedrive::monitor::detail::socket_io_heartbeat_timeout;
using onedrive::monitor::detail::socket_io_notification;
using onedrive::monitor::detail::socket_io_url;
using onedrive::test::fail;
using namespace onedrive::monitor::detail;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error{message};
}

const std::string open_frame =
    R"(0{"sid":"one","pingInterval":25,"pingTimeout":20})";
using Commands = std::vector<ProtocolCommand>;

void test_protocol_handshake_and_heartbeat() {
    SocketProtocol protocol;
    require(
        protocol.phase() == SocketPhase::connecting &&
            protocol.connected().empty() &&
            protocol.phase() == SocketPhase::websocket_connected,
        "WebSocket connection was confused with Engine.IO readiness"
    );
    require(
        protocol.tick(1000ms).empty(),
        "heartbeat deadline existed before Engine.IO open"
    );
    require(
        protocol.text("2probe", true, 0ms) ==
            Commands{{ProtocolEffect::send, "3"}},
        "pre-open Engine.IO ping compatibility changed"
    );
    require(
        protocol.text(open_frame, true, 10ms) ==
                Commands{
                    {ProtocolEffect::send, "40"},
                    {ProtocolEffect::send, "40/notifications"},
                    {ProtocolEffect::connected, {}}
                } &&
            protocol.phase() == SocketPhase::engine_open,
        "Engine.IO open did not emit ordered handshakes and one connected event"
    );
    require(
        protocol.text(open_frame, true, 20ms).empty(),
        "duplicate open replayed handshakes"
    );
    require(
        protocol.tick(55ms).empty(),
        "heartbeat expired at the compatible inclusive boundary"
    );
    require(
        protocol.text("2", true, 55ms) == Commands{{ProtocolEffect::send, "3"}},
        "Engine.IO heartbeat did not emit pong"
    );
    require(
        protocol.text(R"(40{"sid":"root"})", true, 56ms).empty() &&
            protocol.phase() == SocketPhase::ready,
        "root namespace acknowledgement did not enter ready"
    );
    require(
        protocol.text(R"(40/notifications,{"sid":"notifications"})", true, 57ms)
                .empty() &&
            protocol.phase() == SocketPhase::listening,
        "notification namespace acknowledgement did not enter listening"
    );
    require(
        protocol.text("40", true, 58ms).empty() &&
            protocol.phase() == SocketPhase::listening,
        "duplicate root acknowledgement regressed namespace readiness"
    );
    require(
        protocol.text(R"(42/notifications,["notification",{}])", true, 59ms) ==
            Commands{{ProtocolEffect::notification, {}}},
        "listening protocol did not publish notification"
    );
    require(
        protocol.text(R"(42["other",{}])", true, 60ms).empty() &&
            protocol.text("43[]", true, 61ms).empty() &&
            protocol.text("3", true, 62ms).empty() &&
            protocol.text("6", true, 63ms).empty(),
        "unrelated event/ack/pong/noop caused a wakeup"
    );
    require(
        protocol.tick(100ms).empty(),
        "heartbeat expired before its extended deadline"
    );
    require(
        protocol.tick(101ms) ==
                Commands{
                    {ProtocolEffect::close, "Socket.IO heartbeat timed out"},
                    {ProtocolEffect::disconnected, {}}
                } &&
            protocol.phase() == SocketPhase::disconnected,
        "heartbeat deadline did not close and publish disconnection once"
    );
    require(
        protocol.tick(102ms).empty() &&
            protocol.text("2", true, 103ms).empty() &&
            protocol.fail("duplicate failure").empty() &&
            protocol.control(WebSocketControl::close).empty(),
        "terminal protocol emitted duplicate effects"
    );
}

void test_protocol_fragments_and_errors() {
    for (std::size_t split = 0; split <= open_frame.size(); ++split) {
        SocketProtocol protocol;
        protocol.connected();
        require(
            protocol
                .text(std::string_view{open_frame}.substr(0, split), false, 0ms)
                .empty(),
            "partial WebSocket message was parsed early"
        );
        require(
            protocol.control(WebSocketControl::ping).empty() &&
                protocol.control(WebSocketControl::pong).empty(),
            "WebSocket control frames interrupted text reassembly"
        );
        require(
            protocol.text(std::string_view{open_frame}.substr(split), true, 1ms)
                    .size() == 3,
            "fragmented Engine.IO message did not survive chunk boundaries"
        );
        require(
            protocol.text(R"(42["notification",{}])", true, 2ms) ==
                Commands{{ProtocolEffect::notification, {}}},
            "eager notification before namespace ack compatibility changed"
        );
    }
    for (const auto frame : {
             "",
             "unexpected",
             "0not-json",
             "0{}",
             "1",
             "41",
             "41/notifications,",
             R"(44{"message":"denied"})",
             R"(44/notifications,{"message":"denied"})",
         }) {
        SocketProtocol protocol;
        protocol.connected();
        protocol.text(open_frame, true, 0ms);
        const auto effects = protocol.text(frame, true, 1ms);
        require(
            effects.size() == 2 && effects[0].effect == ProtocolEffect::close &&
                !effects[0].text.empty() &&
                effects[1].effect == ProtocolEffect::disconnected &&
                protocol.phase() == SocketPhase::disconnected,
            "malformed/disconnected/rejected frame was silently ignored"
        );
    }
    SocketProtocol unordered;
    unordered.connected();
    require(
        unordered.text("40", true, 0ms).size() == 2,
        "Socket.IO readiness was accepted before Engine.IO open"
    );
    SocketProtocol early;
    require(
        early.text("0{}", true, 0ms).size() == 2,
        "protocol accepted data before WebSocket connection"
    );
    SocketProtocol duplicate;
    duplicate.connected();
    require(
        duplicate.connected().size() == 2,
        "duplicate WebSocket connection was accepted"
    );
    for (const auto control :
         {WebSocketControl::binary, WebSocketControl::close}) {
        SocketProtocol protocol;
        protocol.connected();
        require(
            protocol.control(control).size() == 2,
            "binary/closed WebSocket did not fail explicitly"
        );
    }
    SocketProtocol oversized;
    oversized.connected();
    require(
        oversized
                .text(
                    std::string(SocketProtocol::message_limit, 'x'), false, 0ms
                )
                .empty() &&
            oversized.text("x", true, 1ms).size() == 2,
        "fragmented message size cap was not enforced"
    );
    SocketProtocol unknown_namespace;
    unknown_namespace.connected();
    unknown_namespace.text(open_frame, true, 0ms);
    require(
        unknown_namespace.text("40/other,", true, 1ms).empty() &&
            unknown_namespace.phase() == SocketPhase::engine_open &&
            unknown_namespace.text("40/notifications", true, 2ms).empty() &&
            unknown_namespace.phase() == SocketPhase::listening,
        "unrelated namespace acknowledgement changed notification readiness"
    );
    SocketProtocol malformed_duplicate;
    malformed_duplicate.connected();
    malformed_duplicate.text(open_frame, true, 0ms);
    require(
        malformed_duplicate.text("0{}", true, 1ms).size() == 2,
        "malformed duplicate open bypassed heartbeat validation"
    );
}

void test_protocol_stop() {
    for (const int stage : {0, 1, 2, 3, 4, 5}) {
        SocketProtocol protocol;
        if (stage > 0)
            protocol.connected();
        if (stage > 1)
            protocol.text(open_frame, true, 0ms);
        if (stage > 2)
            protocol.text("40", true, 0ms);
        if (stage > 3)
            protocol.text("40/notifications,", true, 0ms);
        if (stage > 4)
            protocol.text("42[", false, 0ms);
        require(
            protocol.stop() == Commands{{ProtocolEffect::close, {}}} &&
                protocol.phase() == SocketPhase::stopped &&
                protocol.stop().empty() &&
                protocol.text(open_frame, true, 0ms).empty() &&
                protocol.control(WebSocketControl::binary).empty() &&
                protocol.tick(100ms).empty(),
            "stop emitted disconnection, reused secrets, or retained protocol "
            "activity"
        );
    }
    SocketProtocol disconnected;
    disconnected.fail("test");
    require(
        disconnected.stop() == Commands{{ProtocolEffect::close, {}}},
        "stopping a disconnected transport did not enter stopped"
    );
}

int test_url_conversion() {
    const auto converted = socket_io_url(
        "https://notify.example.test/notifications?token=abc&applicationId=def"
    );
    if (converted !=
        "wss://notify.example.test/socket.io/?EIO=4&transport=websocket&"
        "token=abc&applicationId=def") {
        return fail("notification URL was not converted to Socket.IO");
    }
    try {
        static_cast<void>(socket_io_url("not-a-url"));
        return fail("invalid notification URL was accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(socket_io_url("http://notify.example.test/path"));
        return fail("insecure notification URL was accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(socket_io_url("https:///missing-authority"));
        return fail("notification URL without an authority was accepted");
    } catch (const std::invalid_argument&) {
    }
    if (socket_io_url("https://notify.example.test/notifications") !=
        "wss://notify.example.test/socket.io/?EIO=4&transport=websocket") {
        return fail(
            "notification URL without a query was converted incorrectly"
        );
    }
    return EXIT_SUCCESS;
}

int test_open_frame() {
    const auto timeout = socket_io_heartbeat_timeout(
        R"(0{"sid":"one","pingInterval":25000,"pingTimeout":20000})"
    );
    if (timeout != std::chrono::milliseconds{45000}) {
        return fail("Socket.IO heartbeat timeout was not parsed");
    }
    for (const auto frame : {
             "40",
             "0not-json",
             R"(0[])",
             R"(0{"pingTimeout":20000})",
             R"(0{"pingInterval":25000})",
             R"(0{"pingInterval":0,"pingTimeout":20000})",
             R"(0{"pingInterval":25000,"pingTimeout":0})",
             R"(0{"pingInterval":-1,"pingTimeout":20000})",
             R"(0{"pingInterval":"25000","pingTimeout":20000})",
             R"(0{"pingInterval":18446744073709551615,"pingTimeout":1})",
             R"(0{"pingInterval":1,"pingTimeout":18446744073709551615})",
             R"(0{"pingInterval":1,"pingTimeout":9223372036854775807})",
         }) {
        if (socket_io_heartbeat_timeout(frame)) {
            return fail("invalid Socket.IO open frame was accepted");
        }
    }
    if (socket_io_heartbeat_timeout(
            R"(0{"pingInterval":1,"pingTimeout":9223372036854775806})"
        ) != std::chrono::milliseconds::max()) {
        return fail("maximum representable Socket.IO heartbeat was rejected");
    }
    return EXIT_SUCCESS;
}

int test_event_parsing() {
    if (!socket_io_notification(R"(42["notification",{"id":"one"}])") ||
        !socket_io_notification(
            R"(42/notifications,["notification","{\"id\":\"two\"}"])"
        )) {
        return fail("notification event was not recognized");
    }

    for (const auto frame : {
             "0{\"sid\":\"one\"}",
             "2",
             "40",
             R"(42["other",{}])",
             R"(42[])",
             R"(42[1,{}])",
             "42/notifications",
             "42/",
             "42/notifications,",
             "42/notifications,{}",
             "42not-json",
         }) {
        if (socket_io_notification(frame)) {
            return fail("non-notification frame was accepted");
        }
    }
    return EXIT_SUCCESS;
}

int test_transport_failure_event() {
    onedrive::monitor::detail::SocketIoTransport transport{
        {},
        std::chrono::seconds{1},
    };
    transport.connect("https://127.0.0.1:1/notifications?token=test");
    pollfd descriptor{
        .fd = transport.descriptor(),
        .events = POLLIN,
        .revents = 0,
    };
    if (::poll(&descriptor, 1, 2000) != 1 ||
        (descriptor.revents & POLLIN) == 0) {
        transport.disconnect();
        return fail("failed WebSocket connection did not signal Monitor");
    }
    const auto events = transport.drain();
    transport.disconnect();
    if (events !=
        std::vector{onedrive::monitor::detail::SocketEvent::disconnected}) {
        return fail("failed WebSocket connection emitted the wrong event");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    try {
        test_protocol_handshake_and_heartbeat();
        test_protocol_fragments_and_errors();
        test_protocol_stop();
    } catch (const std::exception& error) {
        return fail(error.what());
    }
    if (const int result = test_url_conversion(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_open_frame(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_event_parsing(); result != EXIT_SUCCESS) {
        return result;
    }
    return test_transport_failure_event();
}
