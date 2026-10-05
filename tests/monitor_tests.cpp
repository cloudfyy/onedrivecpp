#include "onedrive/monitor/monitor.hpp"
#include "monitor/termination_signal_mask.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

using onedrive::test::fail;
using onedrive::test::wait_until;

}  // namespace

int main() {
    const onedrive::test::TemporaryDirectory temporary;
    const auto root = temporary.path() / "sync";
    std::filesystem::create_directory(root);

    std::atomic_int local_runs{0};
    onedrive::monitor::Monitor local_monitor{
        root,
        [&] {
            ++local_runs;
            return 0;
        },
        5s,
        60ms
    };
    std::atomic_int local_result{-1};
    std::jthread local_worker{
        [&](std::stop_token stop_token) {
            local_result = local_monitor.run(stop_token);
        }
    };
    if (!wait_until([&] { return local_runs == 1; }, 2s, 5ms)) {
        local_worker.request_stop();
        return fail("monitor did not run its initial synchronization");
    }
    std::this_thread::sleep_for(20ms);

    const auto local_file = root / "local.txt";
    {
        std::ofstream output{local_file};
        output << "one";
    }
    {
        std::ofstream output{local_file, std::ios::app};
        output << "two";
    }
    {
        std::ofstream output{local_file, std::ios::app};
        output << "three";
    }
    if (!wait_until([&] { return local_runs >= 2; }, 2s, 5ms)) {
        local_worker.request_stop();
        return fail("local filesystem changes did not trigger synchronization");
    }
    std::this_thread::sleep_for(150ms);
    if (local_runs != 2) {
        local_worker.request_stop();
        return fail("local filesystem burst was not coalesced");
    }

    const auto nested = root / "new" / "nested";
    std::filesystem::create_directories(nested);
    {
        std::ofstream output{nested / "file.txt"};
        output << "nested";
    }
    if (!wait_until([&] { return local_runs >= 3; }, 2s, 5ms)) {
        local_worker.request_stop();
        return fail("new directory subtree did not trigger synchronization");
    }
    std::this_thread::sleep_for(100ms);
    const int runs_before_symlink = local_runs;
    const auto outside = temporary.path() / "outside";
    std::filesystem::create_directory(outside);
    const auto linked = root / "linked";
    std::filesystem::create_directory_symlink(outside, linked);
    {
        std::ofstream output{outside / "ignored.txt"};
        output << "outside";
    }
    std::this_thread::sleep_for(150ms);
    if (local_runs != runs_before_symlink) {
        local_worker.request_stop();
        return fail("monitor followed a directory symlink");
    }
    local_worker.request_stop();
    local_worker.join();
    if (local_result != 0) {
        return fail("stop token did not end local monitoring successfully");
    }

    std::atomic_int periodic_runs{0};
    onedrive::monitor::Monitor periodic_monitor{
        root,
        [&] {
            const int run = ++periodic_runs;
            return run <= 2 ? 2 : 0;
        },
        60ms,
        10ms
    };
    std::atomic_int periodic_result{-1};
    std::jthread periodic_worker{
        [&](std::stop_token stop_token) {
            periodic_result = periodic_monitor.run(stop_token);
        }
    };
    if (!wait_until([&] { return periodic_runs >= 2; }, 2s, 5ms)) {
        periodic_worker.request_stop();
        return fail("Graph polling interval did not trigger synchronization");
    }
    periodic_worker.request_stop();
    periodic_worker.join();
    if (periodic_result != 0) {
        return fail("periodic monitor did not stop successfully");
    }

    std::atomic_int settled_runs{0};
    std::atomic<std::int64_t> settled_elapsed{-1};
    std::chrono::steady_clock::time_point changed_at;
    onedrive::monitor::Monitor settle_monitor{
        root,
        [&] {
            const int run = ++settled_runs;
            if (run == 2) {
                settled_elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - changed_at
                    ).count();
            }
            return 0;
        },
        80ms,
        180ms
    };
    std::jthread settle_worker{
        [&](std::stop_token stop_token) {
            static_cast<void>(settle_monitor.run(stop_token));
        }
    };
    if (!wait_until([&] { return settled_runs == 1; }, 2s, 5ms)) {
        settle_worker.request_stop();
        return fail("settle test monitor did not become ready");
    }
    std::this_thread::sleep_for(20ms);
    changed_at = std::chrono::steady_clock::now();
    {
        std::ofstream output{root / "settle.txt"};
        output << "settled";
    }
    if (!wait_until([&] { return settled_runs >= 2; }, 2s, 5ms)) {
        settle_worker.request_stop();
        return fail("settled local change did not trigger synchronization");
    }
    settle_worker.request_stop();
    settle_worker.join();
    if (settled_elapsed < 140) {
        return fail("Graph polling interrupted the local settle delay");
    }

    const auto signal_ready = temporary.path() / "signal-ready";
    const pid_t child = ::fork();
    if (child < 0) {
        return fail("could not fork signal-handling monitor test");
    }
    if (child == 0) {
        onedrive::monitor::detail::TerminationSignalMask signal_mask;
        std::jthread preexisting_worker{
            [](std::stop_token stop_token) {
                while (!stop_token.stop_requested()) {
                    std::this_thread::sleep_for(5ms);
                }
            }
        };
        onedrive::monitor::Monitor signal_monitor{
            root,
            [&] {
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
            [&] { return std::filesystem::exists(signal_ready); },
            2s,
            5ms
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
                const pid_t result =
                    ::waitpid(child, &signal_status, WNOHANG);
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
        [&] {
            ++stopped_runs;
            return 0;
        },
        1s,
        10ms
    };
    if (stopped_monitor.run(stopped.get_token()) != 0 ||
        stopped_runs != 0) {
        return fail("pre-stopped monitor ran synchronization");
    }

    std::stop_source stopped_after_initial;
    int initial_runs = 0;
    onedrive::monitor::Monitor stop_after_initial_monitor{
        root,
        [&] {
            ++initial_runs;
            stopped_after_initial.request_stop();
            return 0;
        },
        1s,
        10ms
    };
    if (stop_after_initial_monitor.run(stopped_after_initial.get_token()) != 0 ||
        initial_runs != 1) {
        return fail("monitor did not honor stop after initial synchronization");
    }

    try {
        static_cast<void>(onedrive::monitor::Monitor{
            root,
            {},
            1s,
            10ms
        });
        return fail("empty synchronization callback was accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(onedrive::monitor::Monitor{
            root,
            [] { return 0; },
            0ms,
            10ms
        });
        return fail("zero monitor poll interval was accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(onedrive::monitor::Monitor{
            root,
            [] { return 0; },
            1s,
            -1ms
        });
        return fail("negative monitor settle delay was accepted");
    } catch (const std::invalid_argument&) {
    }

    onedrive::monitor::Monitor missing_root{
        temporary.path() / "missing",
        [] { return 0; },
        1s,
        10ms
    };
    try {
        std::stop_source stop;
        static_cast<void>(missing_root.run(stop.get_token()));
        return fail("missing monitor root was accepted");
    } catch (const std::runtime_error&) {
    }

    return EXIT_SUCCESS;
}
