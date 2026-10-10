#include "monitor/socket.hpp"
#include "support/common.hpp"

#include <curl/curl.h>
#include <curl/websockets.h>

#include <atomic>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <mutex>
#include <poll.h>

namespace {

using namespace onedrive;
using namespace monitor::detail;
using namespace std::chrono_literals;

struct Receive {
    std::string text;
    int flags{CURLWS_TEXT};
    curl_off_t bytesleft{0};
    CURLcode result{CURLE_OK};
    bool metadata{true};
};
enum class SendResult { complete, partial, failure };
struct Script {
    std::mutex mutex;
    std::deque<Receive> receives;
    std::vector<std::string> sends;
    SendResult send_result{SendResult::complete};
};
std::atomic<Script*> active{nullptr};
constexpr int fake_descriptor = 987654;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error{message};
}

void run_script(
    std::deque<Receive> receives,
    std::vector<SocketEvent> expected,
    std::vector<std::string> sends,
    SendResult send_result = SendResult::complete
) {
    Script script;
    script.receives = std::move(receives);
    script.send_result = send_result;
    active = &script;
    SocketIoTransport transport{{}, 1s};
    transport.connect("https://notify.example.test/notifications");
    std::vector<SocketEvent> observed;
    const bool completed = test::wait_until(
        [&] {
            const auto events = transport.drain();
            observed.insert(observed.end(), events.begin(), events.end());
            return observed == expected;
        },
        2s
    );
    transport.disconnect();
    const auto remaining = transport.drain();
    observed.insert(observed.end(), remaining.begin(), remaining.end());
    active = nullptr;
    require(
        completed && observed == expected,
        "production WebSocket adapter emitted incorrect reducer events"
    );
    require(
        script.sends == sends,
        "production WebSocket adapter did not execute ordered send commands"
    );
}

void test_fragmented_transport() {
    run_script(
        {
            {R"(0{"pingInterval":)", CURLWS_TEXT, 17},
            {{}, 0, 0, CURLE_AGAIN},
            {"25,", CURLWS_TEXT | CURLWS_CONT},
            {{}, CURLWS_PING},
            {{}, CURLWS_PONG},
            {R"("pingTimeout":20})"},
            {"40"},
            {"40/notifications,"},
            {"2"},
            {R"(42["notifi)", CURLWS_TEXT | CURLWS_CONT},
            {{}, 0, 0, CURLE_AGAIN},
            {R"(cation",{}])"},
            {"41/notifications,"},
        },
        {SocketEvent::connected,
         SocketEvent::notification,
         SocketEvent::disconnected},
        {"40", "40/notifications", "3"}
    );
}

void test_transport_failures() {
    const std::string open = R"(0{"pingInterval":25,"pingTimeout":20})";
    for (const Receive& failed : {
             Receive{"0{}"},
             Receive{{}, CURLWS_BINARY},
             Receive{{}, CURLWS_CLOSE},
             Receive{{}, 0, 0, CURLE_RECV_ERROR},
             Receive{{}, CURLWS_TEXT, 0, CURLE_OK, false},
             Receive{R"(44{"message":"denied"})"},
         }) {
        run_script({failed}, {SocketEvent::disconnected}, {});
    }
    run_script(
        {{open}}, {SocketEvent::disconnected}, {"40"}, SendResult::partial
    );
    run_script(
        {{open}}, {SocketEvent::disconnected}, {"40"}, SendResult::failure
    );
}

void test_transport_stop() {
    Script script;
    script.receives = {
        {R"(0{"pingInterval":25000,"pingTimeout":20000})"},
        {"42[", CURLWS_TEXT | CURLWS_CONT}
    };
    active = &script;
    SocketIoTransport transport{{}, 1s};
    transport.connect("https://notify.example.test/notifications");
    require(
        test::wait_until(
            [&] {
                const std::scoped_lock lock{script.mutex};
                return script.receives.empty();
            },
            2s
        ),
        "WebSocket adapter did not consume the partial message before stop"
    );
    transport.disconnect();
    const auto events = transport.drain();
    active = nullptr;
    require(
        events == std::vector{SocketEvent::connected},
        "intentional stop published an unexpected disconnection/partial "
        "notification"
    );
}

} // namespace

extern "C" CURLcode __wrap_curl_easy_perform(CURL*) {
    return CURLE_OK;
}

extern "C" CURLcode __real_curl_easy_getinfo(CURL*, CURLINFO, ...);
extern "C" CURLcode __wrap_curl_easy_getinfo(CURL* curl, int info, ...) {
    va_list arguments;
    va_start(arguments, info);
    void* output = va_arg(arguments, void*);
    va_end(arguments);
    if (info == CURLINFO_ACTIVESOCKET) {
        *static_cast<curl_socket_t*>(output) = fake_descriptor;
        return CURLE_OK;
    }
    return __real_curl_easy_getinfo(curl, static_cast<CURLINFO>(info), output);
}

extern "C" int __real_poll(pollfd*, nfds_t, int);
extern "C" int __wrap_poll(pollfd* descriptors, nfds_t count, int timeout) {
    if (count != 1 || descriptors[0].fd != fake_descriptor)
        return __real_poll(descriptors, count, timeout);
    auto* script = active.load();
    {
        const std::scoped_lock lock{script->mutex};
        if (!script->receives.empty()) {
            descriptors[0].revents = POLLIN;
            return 1;
        }
    }
    std::this_thread::sleep_for(1ms);
    return 0;
}

extern "C" CURLcode __wrap_curl_ws_recv(
    CURL*,
    void* buffer,
    std::size_t capacity,
    std::size_t* received,
    const curl_ws_frame** metadata
) {
    auto* script = active.load();
    const std::scoped_lock lock{script->mutex};
    if (script->receives.empty())
        return CURLE_AGAIN;
    const auto next = std::move(script->receives.front());
    script->receives.pop_front();
    if (next.result != CURLE_OK)
        return next.result;
    if (next.text.size() > capacity)
        return CURLE_RECV_ERROR;
    std::memcpy(buffer, next.text.data(), next.text.size());
    *received = next.text.size();
    thread_local curl_ws_frame frame;
    frame = {
        .age = 0,
        .flags = next.flags,
        .offset = 0,
        .bytesleft = next.bytesleft,
        .len = next.text.size()
    };
    *metadata = next.metadata ? &frame : nullptr;
    return CURLE_OK;
}

extern "C" CURLcode __wrap_curl_ws_send(
    CURL*,
    const void* buffer,
    std::size_t size,
    std::size_t* sent,
    curl_off_t,
    unsigned int
) {
    auto* script = active.load();
    const std::scoped_lock lock{script->mutex};
    script->sends.emplace_back(static_cast<const char*>(buffer), size);
    *sent = script->send_result == SendResult::partial ? size - 1 : size;
    return script->send_result == SendResult::failure ? CURLE_SEND_ERROR
                                                      : CURLE_OK;
}

int main() {
    try {
        test_fragmented_transport();
        test_transport_failures();
        test_transport_stop();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        active = nullptr;
        return test::fail(error.what());
    }
}
