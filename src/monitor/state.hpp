#pragma once

#include <chrono>
#include <optional>
#include <utility>
#include <variant>

namespace onedrive::monitor::detail {

using MonitorClock = std::chrono::steady_clock;
using MonitorTimePoint = MonitorClock::time_point;

enum class SynchronizationReason {
    initial,
    local_changes,
    watch_overflow,
    remote_notification,
    graph_poll,
};

enum class LocalChangeKind {
    filesystem,
    watch_overflow,
};

struct StartingState {};

struct IdleState {
    MonitorTimePoint graph_deadline;
};

struct SettlingState {
    MonitorTimePoint local_deadline;
    MonitorTimePoint graph_deadline;
    SynchronizationReason reason;
};

struct SynchronizingState {
    SynchronizationReason reason;
    bool remote_notification_pending{false};
};

struct StoppedState {};

using MonitorState = std::variant<
    StartingState,
    IdleState,
    SettlingState,
    SynchronizingState,
    StoppedState>;

enum class MonitorEffect {
    none,
    synchronize,
    stop,
};

struct MonitorTransition {
    MonitorState state;
    MonitorEffect effect;
};

struct StartEvent {};

struct LocalChangeEvent {
    LocalChangeKind kind;
    MonitorTimePoint observed_at;
};

struct DeadlineReachedEvent {
    MonitorTimePoint now;
};

struct RemoteChangeEvent {};

struct SynchronizationCompletedEvent {
    int status;
    MonitorTimePoint completed_at;
};

struct StopRequestedEvent {};

using MonitorEvent = std::variant<
    StartEvent,
    LocalChangeEvent,
    RemoteChangeEvent,
    DeadlineReachedEvent,
    SynchronizationCompletedEvent,
    StopRequestedEvent>;

struct MonitorTiming {
    std::chrono::milliseconds poll_interval;
    std::chrono::milliseconds settle_delay;
};

template <typename... Callables> struct Overloaded : Callables... {
    using Callables::operator()...;
};

[[nodiscard]] inline SynchronizationReason
synchronization_reason(LocalChangeKind kind) noexcept {
    return kind == LocalChangeKind::watch_overflow
               ? SynchronizationReason::watch_overflow
               : SynchronizationReason::local_changes;
}

[[nodiscard]] inline MonitorTransition transition_monitor(
    MonitorState state, const MonitorEvent& event, MonitorTiming timing
) {
    return std::visit(
        Overloaded{
            [](auto&&, const StopRequestedEvent&) -> MonitorTransition {
                return {
                    .state = StoppedState{},
                    .effect = MonitorEffect::stop,
                };
            },
            [](StartingState, const StartEvent&) -> MonitorTransition {
                return {
                    .state =
                        SynchronizingState{
                            SynchronizationReason::initial,
                        },
                    .effect = MonitorEffect::synchronize,
                };
            },
            [&](IdleState current,
                const LocalChangeEvent& changed) -> MonitorTransition {
                return {
                    .state =
                        SettlingState{
                            .local_deadline =
                                changed.observed_at + timing.settle_delay,
                            .graph_deadline = current.graph_deadline,
                            .reason = synchronization_reason(changed.kind),
                        },
                    .effect = MonitorEffect::none,
                };
            },
            [](IdleState, const RemoteChangeEvent&) -> MonitorTransition {
                return {
                    .state =
                        SynchronizingState{
                            SynchronizationReason::remote_notification,
                        },
                    .effect = MonitorEffect::synchronize,
                };
            },
            [](IdleState current,
               const DeadlineReachedEvent& deadline) -> MonitorTransition {
                if (deadline.now < current.graph_deadline) {
                    return {
                        .state = current,
                        .effect = MonitorEffect::none,
                    };
                }
                return {
                    .state =
                        SynchronizingState{
                            SynchronizationReason::graph_poll,
                        },
                    .effect = MonitorEffect::synchronize,
                };
            },
            [&](SettlingState current,
                const LocalChangeEvent& changed) -> MonitorTransition {
                const auto reason =
                    current.reason == SynchronizationReason::watch_overflow ||
                            changed.kind == LocalChangeKind::watch_overflow
                        ? SynchronizationReason::watch_overflow
                        : SynchronizationReason::local_changes;
                return {
                    .state =
                        SettlingState{
                            .local_deadline =
                                changed.observed_at + timing.settle_delay,
                            .graph_deadline = current.graph_deadline,
                            .reason = reason,
                        },
                    .effect = MonitorEffect::none,
                };
            },
            [](SettlingState current,
               const DeadlineReachedEvent& deadline) -> MonitorTransition {
                if (deadline.now < current.local_deadline) {
                    return {
                        .state = current,
                        .effect = MonitorEffect::none,
                    };
                }
                return {
                    .state = SynchronizingState{current.reason},
                    .effect = MonitorEffect::synchronize,
                };
            },
            [](SynchronizingState current,
               const RemoteChangeEvent&) -> MonitorTransition {
                current.remote_notification_pending = true;
                return {
                    .state = current,
                    .effect = MonitorEffect::none,
                };
            },
            [&](SynchronizingState current,
                const SynchronizationCompletedEvent& completed)
                -> MonitorTransition {
                if (current.remote_notification_pending) {
                    return {
                        .state =
                            SynchronizingState{
                                SynchronizationReason::remote_notification,
                            },
                        .effect = MonitorEffect::synchronize,
                    };
                }
                return {
                    .state =
                        IdleState{
                            completed.completed_at + timing.poll_interval,
                        },
                    .effect = MonitorEffect::none,
                };
            },
            [](auto&& current, const auto&) -> MonitorTransition {
                return {
                    .state = std::forward<decltype(current)>(current),
                    .effect = MonitorEffect::none,
                };
            },
        },
        state,
        event
    );
}

[[nodiscard]] inline std::optional<MonitorTimePoint>
next_deadline(const MonitorState& state) noexcept {
    if (const auto* idle = std::get_if<IdleState>(&state)) {
        return idle->graph_deadline;
    }
    if (const auto* settling = std::get_if<SettlingState>(&state)) {
        return settling->local_deadline;
    }
    return std::nullopt;
}

[[nodiscard]] inline const char*
synchronization_reason_name(SynchronizationReason reason) noexcept {
    switch (reason) {
    case SynchronizationReason::initial:
        return "initial monitor startup";
    case SynchronizationReason::local_changes:
        return "local filesystem changes";
    case SynchronizationReason::watch_overflow:
        return "inotify queue overflow";
    case SynchronizationReason::remote_notification:
        return "a remote change notification";
    case SynchronizationReason::graph_poll:
        return "the Graph polling interval";
    }
    std::unreachable();
}

} // namespace onedrive::monitor::detail
