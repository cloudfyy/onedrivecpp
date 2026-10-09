#include "support.hpp"

namespace {

using namespace onedrive::test::monitor;

int test_notification_authentication_and_connection() {
    MonitorFixture fixture;
    std::atomic_int sync_runs{0};
    std::atomic_int channel_requests{0};
    std::atomic_int token_refreshes{0};
    onedrive::monitor::Monitor monitor{
        fixture.root,
        [&](const std::stop_token&) {
            ++sync_runs;
            return 0;
        },
        5s,
        20ms,
        {
            .acquire_channel =
                [&]() -> onedrive::monitor::NotificationChannelResult {
                if (++channel_requests == 1) {
                    return std::unexpected{true};
                }
                return onedrive::monitor::NotificationChannel{
                    .url = "https://127.0.0.1:1/notifications?token=test",
                    .renew_at = std::chrono::steady_clock::now() + 1h,
                };
            },
            .refresh_token =
                [&] {
                    ++token_refreshes;
                    return true;
                },
            .proxy = {},
        },
    };
    std::atomic_int result{-1};
    std::jthread worker{[&](std::stop_token stop_token) {
        result = monitor.run(stop_token);
    }};
    if (!wait_until(
            [&] {
                return sync_runs == 1 && channel_requests >= 2 &&
                       token_refreshes == 1;
            },
            2s,
            5ms
        )) {
        worker.request_stop();
        return fail(
            "Monitor did not refresh authentication and acquire a channel"
        );
    }
    worker.request_stop();
    worker.join();
    if (result != 0) {
        return fail("Monitor did not stop after notification setup");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_notification_authentication_and_connection();
}
