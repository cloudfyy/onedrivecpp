#include "support/progress.hpp"
#include "support.hpp"
#include "onedrive/exit_code.hpp"

namespace {

namespace exit_code = onedrive::exit_code;

using namespace onedrive::test::monitor;

int test_multiple_channels_and_local_roots() {
    MonitorFixture fixture;
    const auto first = fixture.root / "first";
    const auto second = fixture.root / "second";
    std::filesystem::create_directory(first);
    std::filesystem::create_directory(second);
    std::atomic_int rounds{0};
    std::atomic_int first_requests{0};
    std::atomic_int second_requests{0};
    const auto callbacks = [](std::atomic_int& requests) {
        return onedrive::monitor::NotificationCallbacks{
            .acquire_channel =
                [&requests]() -> onedrive::monitor::NotificationChannelResult {
                ++requests;
                return std::unexpected{
                    onedrive::monitor::NotificationFailure::transient
                };
            },
            .refresh_token = [] { return true; },
            .proxy = {},
            .initial_backoff = 10ms,
            .maximum_backoff = 20ms,
        };
    };
    onedrive::monitor::Monitor monitor{
        fixture.root,
        [&](const std::stop_token&) {
            ++rounds;
            return exit_code::success;
        },
        5s,
        10ms,
        callbacks(first_requests),
        {},
        {callbacks(second_requests)}
    };
    std::atomic_int result{-1};
    std::jthread worker{[&](std::stop_token token) {
        result = monitor.run(token);
    }};
    bool ready = wait_until(
        [&] {
            return rounds >= 1 && first_requests >= 2 && second_requests >= 2;
        },
        2s,
        5ms
    );
    if (ready) {
        onedrive::test::write_file(first / "file.txt", "first");
        ready = wait_until([&] { return rounds >= 2; }, 2s, 5ms);
    }
    if (ready) {
        onedrive::test::write_file(second / "file.txt", "second");
        ready = wait_until([&] { return rounds >= 3; }, 2s, 5ms);
    }
    worker.request_stop();
    worker.join();
    return ready && result == exit_code::success
               ? EXIT_SUCCESS
               : fail(
                     "notification retries or local changes omitted one "
                     "configured Drive"
                 );
}

int test_polling_after_notification_unavailable() {
    MonitorFixture fixture;
    std::atomic_int rounds{0};
    std::atomic_int unavailable_requests{0};
    std::atomic_int transient_requests{0};
    std::atomic_int refreshes{0};
    const auto callbacks = [&](bool unavailable) {
        return onedrive::monitor::NotificationCallbacks{
            .acquire_channel = [&, unavailable]()
                -> onedrive::monitor::NotificationChannelResult {
                if (unavailable) {
                    ++unavailable_requests;
                    return std::unexpected{
                        onedrive::monitor::NotificationFailure::unavailable
                    };
                }
                ++transient_requests;
                return std::unexpected{
                    onedrive::monitor::NotificationFailure::transient
                };
            },
            .refresh_token =
                [&] {
                    ++refreshes;
                    return true;
                },
            .initial_backoff = 10ms,
            .maximum_backoff = 20ms,
        };
    };
    onedrive::monitor::Monitor monitor{
        fixture.root,
        [&](const std::stop_token&) {
            ++rounds;
            return exit_code::success;
        },
        30ms,
        1ms,
        callbacks(true),
        {},
        {callbacks(false)},
    };
    std::atomic_int result{-1};
    std::jthread worker{[&](std::stop_token token) {
        result = monitor.run(token);
    }};
    const bool ready = wait_until(
        [&] { return rounds >= 3 && transient_requests >= 3; }, 2s, 5ms
    );
    worker.request_stop();
    worker.join();
    if (!ready || result != exit_code::success || unavailable_requests != 1 ||
        refreshes != 0) {
        return fail(
            "an unavailable Drive retried or stopped polling and other Drive "
            "notifications"
        );
    }
    return EXIT_SUCCESS;
}

int test_invalid_notification_endpoint_keeps_polling() {
    MonitorFixture fixture;
    std::atomic_int rounds{0};
    std::atomic_int requests{0};
    onedrive::monitor::Monitor monitor{
        fixture.root,
        [&](const std::stop_token&) {
            ++rounds;
            return exit_code::success;
        },
        30ms,
        1ms,
        {
            .acquire_channel =
                [&]() -> onedrive::monitor::NotificationChannelResult {
                ++requests;
                return onedrive::monitor::NotificationChannel{
                    .url = "http://invalid.example.test/response-secret",
                };
            },
            .refresh_token = [] { return true; },
            .initial_backoff = 10ms,
            .maximum_backoff = 20ms,
        },
    };
    std::atomic_int result{-1};
    std::jthread worker{[&](std::stop_token token) {
        result = monitor.run(token);
    }};
    const bool ready =
        wait_until([&] { return rounds >= 3 && requests >= 2; }, 2s, 5ms);
    worker.request_stop();
    worker.join();
    return ready && result == exit_code::success
               ? EXIT_SUCCESS
               : fail("invalid notification endpoint terminated polling");
}

int test_backoff_starts_after_request_finishes() {
    for (const bool refresh_failure : {false, true}) {
        MonitorFixture fixture;
        std::atomic_int rounds{0};
        std::atomic_int requests{0};
        std::atomic_int refreshes{0};
        std::atomic_int attempts{0};
        std::atomic_bool premature{false};
        std::chrono::steady_clock::time_point completed;
        const auto delayed_attempt = [&] {
            if (++attempts == 1) {
                std::this_thread::sleep_for(60ms);
                completed = std::chrono::steady_clock::now();
                return false;
            }
            premature = std::chrono::steady_clock::now() - completed < 100ms;
            return true;
        };
        onedrive::monitor::Monitor monitor{
            fixture.root,
            [&](const std::stop_token&) {
                ++rounds;
                return exit_code::success;
            },
            20ms,
            1ms,
            {
                .acquire_channel =
                    [&]() -> onedrive::monitor::NotificationChannelResult {
                    ++requests;
                    if (refresh_failure) {
                        return std::unexpected{
                            requests == 1
                                ? onedrive::monitor::NotificationFailure::
                                      unauthorized
                                : onedrive::monitor::NotificationFailure::
                                      unavailable
                        };
                    }
                    return std::unexpected{
                        delayed_attempt()
                            ? onedrive::monitor::NotificationFailure::
                                  unavailable
                            : onedrive::monitor::NotificationFailure::transient
                    };
                },
                .refresh_token =
                    [&] {
                        ++refreshes;
                        return refresh_failure ? delayed_attempt() : true;
                    },
                .initial_backoff = 100ms,
                .maximum_backoff = 100ms,
            },
        };
        std::atomic_int result{-1};
        std::jthread worker{[&](std::stop_token token) {
            result = monitor.run(token);
        }};
        const bool ready = wait_until([&] { return attempts >= 2; }, 2s, 5ms);
        worker.request_stop();
        worker.join();
        if (!ready || premature || rounds < 2 || result != exit_code::success ||
            requests != 2 || refreshes != (refresh_failure ? 2 : 0)) {
            return fail(
                "notification request time consumed its required retry backoff"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_notification_authentication_and_connection() {
    MonitorFixture fixture;
    std::atomic_int sync_runs{0};
    std::atomic_int channel_requests{0};
    std::atomic_int token_refreshes{0};
    onedrive::monitor::Monitor monitor{
        fixture.root,
        [&](const std::stop_token&) {
            ++sync_runs;
            return exit_code::success;
        },
        5s,
        20ms,
        {
            .acquire_channel =
                [&]() -> onedrive::monitor::NotificationChannelResult {
                const int request = ++channel_requests;
                if (request == 1) {
                    return std::unexpected{
                        onedrive::monitor::NotificationFailure::unauthorized
                    };
                }
                if (request == 2) {
                    return std::unexpected{
                        onedrive::monitor::NotificationFailure::transient
                    };
                }
                return onedrive::monitor::NotificationChannel{
                    .url = "https://127.0.0.1:1/notifications?token=test",
                    .renew_at = std::chrono::steady_clock::now() + 1h,
                };
            },
            .refresh_token = [&] { return ++token_refreshes > 1; },
            .proxy = {},
            .connect_timeout = 100ms,
            .initial_backoff = 10ms,
            .maximum_backoff = 20ms,
        },
    };
    std::atomic_int result{-1};
    std::jthread worker{[&](std::stop_token stop_token) {
        result = monitor.run(stop_token);
    }};
    if (!wait_until(
            [&] {
                return sync_runs == 1 && channel_requests >= 4 &&
                       token_refreshes >= 2;
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
    if (result != exit_code::success) {
        return fail("Monitor did not stop after notification setup");
    }
    return EXIT_SUCCESS;
}

int test_notification_retry_after_transient_failures() {
    MonitorFixture fixture;
    std::atomic_int sync_runs{0};
    std::atomic_int channel_requests{0};
    std::atomic_int token_refreshes{0};
    onedrive::monitor::Monitor monitor{
        fixture.root,
        [&](const std::stop_token&) {
            ++sync_runs;
            return exit_code::success;
        },
        5s,
        20ms,
        {
            .acquire_channel =
                [&]() -> onedrive::monitor::NotificationChannelResult {
                const int request = ++channel_requests;
                if (request == 1) {
                    return std::unexpected{
                        onedrive::monitor::NotificationFailure::transient
                    };
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
            .connect_timeout = 100ms,
            .initial_backoff = 10ms,
            .maximum_backoff = 20ms,
        },
    };
    std::atomic_int result{-1};
    std::jthread worker{[&](std::stop_token stop_token) {
        result = monitor.run(stop_token);
    }};
    if (!wait_until(
            [&] {
                return sync_runs == 1 && channel_requests >= 2 &&
                       token_refreshes == 0;
            },
            2s,
            5ms
        )) {
        worker.request_stop();
        worker.join();
        return fail("transient notification failure did not retry acquisition");
    }
    worker.request_stop();
    worker.join();
    if (result != exit_code::success) {
        return fail("monitor did not stop after transient notification retry");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    onedrive::test::ScenarioProgress progress{6};
    if (progress.run(
            "backoff_starts_after_request_finishes",
            test_backoff_starts_after_request_finishes
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (progress.run(
            "polling_after_notification_unavailable",
            test_polling_after_notification_unavailable
        ) != EXIT_SUCCESS ||
        progress.run(
            "invalid_notification_endpoint_keeps_polling",
            test_invalid_notification_endpoint_keeps_polling
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (progress.run(
            "multiple_channels_and_local_roots",
            test_multiple_channels_and_local_roots
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (const int result = progress.run(
            "notification_authentication_and_connection",
            test_notification_authentication_and_connection
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    return progress.finish(progress.run(
        "notification_retry_after_transient_failures",
        test_notification_retry_after_transient_failures
    ));
}
