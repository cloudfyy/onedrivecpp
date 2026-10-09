#include "support.hpp"

namespace {

using namespace onedrive::test::monitor;

int test_stop() {
    MonitorFixture fixture;
    const auto& temporary = fixture.temporary;
    const auto& root = fixture.root;
    const auto signal_ready = temporary.path() / "signal-ready";
    const pid_t child = ::fork();
    if (child < 0) {
        return fail("could not fork signal-handling monitor test");
    }
    if (child == 0) {
        onedrive::monitor::detail::TerminationSignalMask signal_mask;
        std::jthread preexisting_worker{[](std::stop_token stop_token) {
            while (!stop_token.stop_requested()) {
                std::this_thread::sleep_for(5ms);
            }
        }};
        onedrive::monitor::Monitor signal_monitor{
            root,
            [&](const std::stop_token&) {
                std::ofstream{signal_ready} << "ready";
                return 0;
            },
            5s,
            10ms
        };
        const int result = signal_monitor.run();
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (!wait_until(
            [&] { return std::filesystem::exists(signal_ready); }, 2s, 5ms
        )) {
        static_cast<void>(::kill(child, SIGKILL));
        static_cast<void>(::waitpid(child, nullptr, 0));
        return fail("signal-handling monitor did not become ready");
    }
    if (::kill(child, SIGTERM) < 0) {
        static_cast<void>(::kill(child, SIGKILL));
        static_cast<void>(::waitpid(child, nullptr, 0));
        return fail("could not send SIGTERM to monitor");
    }
    int signal_status = 0;
    bool signal_stopped = false;
    if (!wait_until(
            [&] {
                const pid_t result = ::waitpid(child, &signal_status, WNOHANG);
                signal_stopped = result == child;
                return signal_stopped;
            },
            2s,
            5ms
        )) {
        static_cast<void>(::kill(child, SIGKILL));
        static_cast<void>(::waitpid(child, nullptr, 0));
        return fail("SIGTERM did not stop monitor");
    }
    if (!WIFEXITED(signal_status) ||
        WEXITSTATUS(signal_status) != EXIT_SUCCESS) {
        return fail("SIGTERM did not stop monitor successfully");
    }

    std::stop_source stopped;
    stopped.request_stop();
    int stopped_runs = 0;
    onedrive::monitor::Monitor stopped_monitor{
        root,
        [&](const std::stop_token&) {
            ++stopped_runs;
            return 0;
        },
        1s,
        10ms
    };
    if (stopped_monitor.run(false, stopped.get_token()) != 0 ||
        stopped_runs != 0) {
        return fail("pre-stopped monitor ran synchronization");
    }

    std::stop_source stopped_after_initial;
    int initial_runs = 0;
    onedrive::monitor::Monitor stop_after_initial_monitor{
        root,
        [&](const std::stop_token&) {
            ++initial_runs;
            stopped_after_initial.request_stop();
            return 0;
        },
        1s,
        10ms
    };
    if (stop_after_initial_monitor.run(stopped_after_initial.get_token()) !=
            0 ||
        initial_runs != 1) {
        return fail("monitor did not honor stop after initial synchronization");
    }

    std::stop_source cancel_during_sync;
    onedrive::monitor::Monitor cancel_sync_monitor{
        root,
        [](const std::stop_token& stop_token) {
            while (!stop_token.stop_requested()) {
                std::this_thread::sleep_for(1ms);
            }
            return 130;
        },
        1s,
        10ms
    };
    std::jthread cancel_sync{[&] {
        std::this_thread::sleep_for(10ms);
        cancel_during_sync.request_stop();
    }};
    if (cancel_sync_monitor.run(cancel_during_sync.get_token()) != 0) {
        return fail("monitor did not finish after cancelling synchronization");
    }
    cancel_sync.join();

    try {
        static_cast<void>(onedrive::monitor::Monitor{root, {}, 1s, 10ms});
        return fail("empty synchronization callback was accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(
            onedrive::monitor::Monitor{
                root,
                [](const std::stop_token&) { return 0; },
                0ms,
                10ms
            }
        );
        return fail("zero monitor poll interval was accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(
            onedrive::monitor::Monitor{
                root,
                [](const std::stop_token&) { return 0; },
                1s,
                -1ms
            }
        );
        return fail("negative monitor settle delay was accepted");
    } catch (const std::invalid_argument&) {
    }

    onedrive::monitor::Monitor missing_root{
        temporary.path() / "missing",
        [](const std::stop_token&) { return 0; },
        1s,
        10ms
    };
    try {
        std::stop_source stop;
        static_cast<void>(missing_root.run(stop.get_token()));
        return fail("missing monitor root was accepted");
    } catch (const std::runtime_error&) {
    }

    const auto regular_file = temporary.path() / "not-a-directory";
    std::ofstream{regular_file} << "file";
    onedrive::monitor::Monitor regular_file_root{
        regular_file, [](const std::stop_token&) { return 0; }, 1s, 10ms
    };
    try {
        static_cast<void>(regular_file_root.run(std::stop_token{}));
        return fail("regular file was accepted as the monitor root");
    } catch (const std::runtime_error&) {
    }

    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_stop();
}
