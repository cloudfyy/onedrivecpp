#include "onedrive/monitor/monitor.hpp"
#include "monitor/state_machine.hpp"
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
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

using onedrive::test::fail;
using onedrive::test::wait_until;

int test_state_machine() {
    using namespace onedrive::monitor::detail;

    const MonitorTiming timing{
        .poll_interval = 100ms,
        .settle_delay = 20ms,
    };
    const MonitorTimePoint origin{};
    MonitorState state = StartingState{};
    if (next_deadline(state)) {
        return fail("starting monitor unexpectedly had a deadline");
    }
    auto ignored = transition_monitor(
        state,
        DeadlineReachedEvent{origin},
        timing
    );
    if (ignored.effect != MonitorEffect::none ||
        !std::holds_alternative<StartingState>(ignored.state)) {
        return fail("starting monitor accepted an invalid deadline event");
    }

    auto transition = transition_monitor(
        std::move(state),
        StartEvent{},
        timing
    );
    if (transition.effect != MonitorEffect::synchronize ||
        std::get<SynchronizingState>(transition.state).reason !=
            SynchronizationReason::initial) {
        return fail("monitor did not start with initial synchronization");
    }
    ignored = transition_monitor(
        transition.state,
        DeadlineReachedEvent{origin},
        timing
    );
    if (ignored.effect != MonitorEffect::none ||
        !std::holds_alternative<SynchronizingState>(ignored.state)) {
        return fail("synchronizing monitor accepted a deadline event");
    }
    state = transition_monitor(
        std::move(transition.state),
        SynchronizationCompletedEvent{
            .status = 0,
            .completed_at = origin + 10ms,
        },
        timing
    ).state;
    const auto* idle = std::get_if<IdleState>(&state);
    if (idle == nullptr || idle->graph_deadline != origin + 110ms ||
        next_deadline(state) != idle->graph_deadline) {
        return fail("monitor did not schedule its first Graph poll");
    }

    transition = transition_monitor(
        state,
        DeadlineReachedEvent{origin + 109ms},
        timing
    );
    if (transition.effect != MonitorEffect::none ||
        !std::holds_alternative<IdleState>(transition.state)) {
        return fail("monitor synchronized before its Graph deadline");
    }
    transition = transition_monitor(
        state,
        LocalChangeEvent{
            .kind = LocalChangeKind::filesystem,
            .observed_at = origin + 15ms,
        },
        timing
    );
    auto* settling = std::get_if<SettlingState>(&transition.state);
    if (settling == nullptr ||
        settling->local_deadline != origin + 35ms ||
        settling->graph_deadline != origin + 110ms ||
        settling->reason != SynchronizationReason::local_changes) {
        return fail("monitor did not begin settling a local change");
    }

    transition = transition_monitor(
        std::move(transition.state),
        LocalChangeEvent{
            .kind = LocalChangeKind::filesystem,
            .observed_at = origin + 20ms,
        },
        timing
    );
    settling = std::get_if<SettlingState>(&transition.state);
    if (settling == nullptr ||
        settling->local_deadline != origin + 40ms) {
        return fail("monitor did not extend its local settle deadline");
    }
    transition = transition_monitor(
        std::move(transition.state),
        LocalChangeEvent{
            .kind = LocalChangeKind::watch_overflow,
            .observed_at = origin + 25ms,
        },
        timing
    );
    transition = transition_monitor(
        std::move(transition.state),
        LocalChangeEvent{
            .kind = LocalChangeKind::filesystem,
            .observed_at = origin + 30ms,
        },
        timing
    );
    settling = std::get_if<SettlingState>(&transition.state);
    if (settling == nullptr ||
        settling->local_deadline != origin + 50ms ||
        settling->reason != SynchronizationReason::watch_overflow ||
        next_deadline(transition.state) != origin + 50ms) {
        return fail("monitor did not preserve an overflow while settling");
    }

    transition = transition_monitor(
        std::move(transition.state),
        DeadlineReachedEvent{origin + 49ms},
        timing
    );
    if (transition.effect != MonitorEffect::none ||
        !std::holds_alternative<SettlingState>(transition.state)) {
        return fail("monitor synchronized before its settle deadline");
    }
    transition = transition_monitor(
        std::move(transition.state),
        DeadlineReachedEvent{origin + 110ms},
        timing
    );
    if (transition.effect != MonitorEffect::synchronize ||
        std::get<SynchronizingState>(transition.state).reason !=
            SynchronizationReason::watch_overflow) {
        return fail("local settling did not take priority over Graph polling");
    }

    state = transition_monitor(
        IdleState{origin + 60ms},
        DeadlineReachedEvent{origin + 60ms},
        timing
    ).state;
    if (std::get<SynchronizingState>(state).reason !=
        SynchronizationReason::graph_poll) {
        return fail("Graph deadline did not trigger synchronization");
    }
    state = transition_monitor(
        std::move(state),
        SynchronizationCompletedEvent{
            .status = 2,
            .completed_at = origin + 70ms,
        },
        timing
    ).state;
    if (std::get<IdleState>(state).graph_deadline != origin + 170ms) {
        return fail("completed synchronization did not reset Graph polling");
    }
    ignored = transition_monitor(state, StartEvent{}, timing);
    if (ignored.effect != MonitorEffect::none ||
        !std::holds_alternative<IdleState>(ignored.state)) {
        return fail("idle monitor restarted unexpectedly");
    }
    ignored = transition_monitor(
        SettlingState{
            .local_deadline = origin + 180ms,
            .graph_deadline = origin + 170ms,
            .reason = SynchronizationReason::local_changes,
        },
        StartEvent{},
        timing
    );
    if (ignored.effect != MonitorEffect::none ||
        !std::holds_alternative<SettlingState>(ignored.state)) {
        return fail("settling monitor restarted unexpectedly");
    }
    ignored = transition_monitor(
        StoppedState{},
        DeadlineReachedEvent{origin},
        timing
    );
    if (ignored.effect != MonitorEffect::none ||
        !std::holds_alternative<StoppedState>(ignored.state)) {
        return fail("stopped monitor accepted a deadline event");
    }

    for (MonitorState stoppable : {
             MonitorState{StartingState{}},
             MonitorState{IdleState{origin}},
             MonitorState{SettlingState{
                 origin,
                 origin,
                 SynchronizationReason::local_changes,
             }},
             MonitorState{SynchronizingState{
                 SynchronizationReason::graph_poll,
             }},
             MonitorState{StoppedState{}},
         }) {
        transition = transition_monitor(
            std::move(stoppable),
            StopRequestedEvent{},
            timing
        );
        if (transition.effect != MonitorEffect::stop ||
            !std::holds_alternative<StoppedState>(transition.state) ||
            next_deadline(transition.state)) {
            return fail("monitor state did not stop cleanly");
        }
    }

    if (std::string_view{synchronization_reason_name(
            SynchronizationReason::initial
        )}.empty() ||
        std::string_view{synchronization_reason_name(
            SynchronizationReason::local_changes
        )}.empty() ||
        std::string_view{synchronization_reason_name(
            SynchronizationReason::watch_overflow
        )}.empty() ||
        std::string_view{synchronization_reason_name(
            SynchronizationReason::graph_poll
        )}.empty()) {
        return fail("monitor synchronization reason was not printable");
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (const int result = test_state_machine(); result != EXIT_SUCCESS) {
        return result;
    }

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
