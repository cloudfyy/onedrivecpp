#include "support.hpp"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <sstream>

int main() {
    using namespace onedrive::test::monitor;
    std::size_t capacity{};
    std::ifstream capacity_file{"/proc/sys/fs/inotify/max_queued_events"};
    if (!(capacity_file >> capacity) || capacity == 0) {
        return fail("cannot read the inotify queue capacity");
    }
    if (capacity > 65536) {
        std::cerr << "Skipping inotify overflow: queue capacity exceeds the "
                     "65536-file fixture budget\n";
        return 77;
    }
    MonitorFixture fixture;
    std::ostringstream messages;
    const auto previous_logger = spdlog::default_logger();
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(messages);
    spdlog::set_default_logger(
        std::make_shared<spdlog::logger>("overflow-test", sink)
    );

    std::atomic_int sync_runs{0};
    bool produced_burst = false;
    const auto nested = fixture.root / "nested";
    const onedrive::monitor::Monitor monitor{
        fixture.root,
        [&](const std::stop_token&) {
            ++sync_runs;
            return 0;
        },
        1h,
        10ms,
        {
            .acquire_channel =
                [&]() -> onedrive::monitor::NotificationChannelResult {
                if (!produced_burst) {
                    produced_burst = true;
                    // The main loop cannot drain its installed watches during
                    // this callback.
                    for (std::size_t index = 0; index <= capacity; ++index) {
                        onedrive::test::write_file(
                            fixture.root / std::to_string(index), ""
                        );
                    }
                    std::filesystem::create_directory(nested);
                }
                return std::unexpected{false};
            },
            .refresh_token = [] { return true; },
            .initial_backoff = 1h,
            .maximum_backoff = 1h,
        },
    };
    std::atomic_int result{-1};
    std::exception_ptr error;
    std::jthread worker{[&](std::stop_token token) {
        try {
            result = monitor.run(token);
        } catch (...) {
            error = std::current_exception();
        }
    }};
    const bool resynchronized =
        wait_until([&] { return sync_runs >= 2; }, 10s, 10ms);
    bool nested_watched = false;
    if (resynchronized) {
        nested_watched = wait_until(
            [&] {
                onedrive::test::write_file(
                    nested / "after-overflow", "changed"
                );
                return sync_runs >= 3;
            },
            3s,
            30ms
        );
    }
    worker.request_stop();
    worker.join();
    spdlog::set_default_logger(previous_logger);
    if (error) {
        std::rethrow_exception(error);
    }
    if (!resynchronized ||
        !messages.str().contains("inotify queue overflowed")) {
        return fail(
            "real inotify overflow did not trigger full resynchronization"
        );
    }
    if (!nested_watched || result != 0) {
        return fail(
            "monitor did not restore subtree watches and stop after overflow"
        );
    }
    return EXIT_SUCCESS;
}
