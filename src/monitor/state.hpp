#pragma once

#include <algorithm>
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
    manual_request,
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
    bool manual_request_pending{false};
    bool local_change_pending{false};
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
struct ManualTriggerEvent {};

struct SynchronizationCompletedEvent {
    int status;
    MonitorTimePoint completed_at;
};

struct StopRequestedEvent {};

using MonitorEvent = std::variant<
    StartEvent,
    LocalChangeEvent,
    RemoteChangeEvent,
    ManualTriggerEvent,
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
            [](SettlingState, const RemoteChangeEvent&) -> MonitorTransition {
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
                if (deadline.now < current.local_deadline &&
                    deadline.now < current.graph_deadline) {
                    return {
                        .state = current,
                        .effect = MonitorEffect::none,
                    };
                }
                return {
                    .state =
                        SynchronizingState{
                            deadline.now >= current.local_deadline
                                ? current.reason
                                : SynchronizationReason::graph_poll
                        },
                    .effect = MonitorEffect::synchronize,
                };
            },
            [](SynchronizingState current,
               const ManualTriggerEvent&) -> MonitorTransition {
                current.manual_request_pending = true;
                return {.state = current, .effect = MonitorEffect::none};
            },
            [](SynchronizingState current,
               const LocalChangeEvent&) -> MonitorTransition {
                current.local_change_pending = true;
                return {.state = current, .effect = MonitorEffect::none};
            },
            [](StoppedState current,
               const ManualTriggerEvent&) -> MonitorTransition {
                return {.state = current, .effect = MonitorEffect::none};
            },
            [](auto&&, const ManualTriggerEvent&) -> MonitorTransition {
                return {
                    .state =
                        SynchronizingState{
                            SynchronizationReason::manual_request
                        },
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
                if (current.manual_request_pending ||
                    current.local_change_pending ||
                    current.remote_notification_pending) {
                    return {
                        .state =
                            SynchronizingState{
                                current.manual_request_pending
                                    ? SynchronizationReason::manual_request
                                : current.local_change_pending
                                    ? SynchronizationReason::local_changes
                                    : SynchronizationReason::
                                          remote_notification,
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

[[nodiscard]] inline MonitorTransition resolve_poll_events(
    MonitorState state,
    bool manual_triggered,
    bool& notification_sync_requested,
    MonitorTimePoint now,
    MonitorTiming timing
) {
    if (manual_triggered) {
        notification_sync_requested = false;
        return transition_monitor(
            std::move(state), ManualTriggerEvent{}, timing
        );
    }
    if (notification_sync_requested) {
        notification_sync_requested = false;
        return transition_monitor(
            std::move(state), RemoteChangeEvent{}, timing
        );
    }
    auto transition = transition_monitor(
        std::move(state), DeadlineReachedEvent{now}, timing
    );
    if (transition.effect == MonitorEffect::synchronize) {
        notification_sync_requested = false;
    }
    return transition;
}

[[nodiscard]] inline std::optional<MonitorTimePoint>
next_deadline(const MonitorState& state) noexcept {
    if (const auto* idle = std::get_if<IdleState>(&state)) {
        return idle->graph_deadline;
    }
    if (const auto* settling = std::get_if<SettlingState>(&state)) {
        return std::min(settling->local_deadline, settling->graph_deadline);
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
    case SynchronizationReason::manual_request:
        return "an explicit Monitor trigger";
    }
    std::unreachable();
}

} // namespace onedrive::monitor::detail
