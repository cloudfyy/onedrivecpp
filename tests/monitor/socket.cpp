#include "monitor/socket.hpp"
#include "support/common.hpp"

#include <cstdlib>
#include <poll.h>
#include <stdexcept>

namespace {

using onedrive::monitor::detail::socket_io_heartbeat_timeout;
using onedrive::monitor::detail::socket_io_notification;
using onedrive::monitor::detail::socket_io_url;
using onedrive::test::fail;

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
             R"(0{"pingInterval":25000})",
             R"(0{"pingInterval":0,"pingTimeout":20000})",
             R"(0{"pingInterval":"25000","pingTimeout":20000})",
         }) {
        if (socket_io_heartbeat_timeout(frame)) {
            return fail("invalid Socket.IO open frame was accepted");
        }
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
             "42/notifications",
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
    onedrive::monitor::detail::SocketIoTransport transport{{}};
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
