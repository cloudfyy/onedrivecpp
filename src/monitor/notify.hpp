#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace onedrive::monitor::detail {

using NotificationClock = std::chrono::steady_clock;
using NotificationTimePoint = NotificationClock::time_point;

struct NotificationTiming {
    std::chrono::milliseconds request_timeout;
    std::chrono::milliseconds connect_timeout;
    std::chrono::milliseconds initial_backoff;
    std::chrono::milliseconds maximum_backoff;
};

enum class NotificationCommand {
    none,
    acquire_channel,
    renew_channel,
    connect_socket,
    refresh_token,
    disconnect_socket,
};

enum class NotificationFailure {
    transient,
    unauthorized,
};

enum class NotificationRetryTarget {
    acquire_channel,
    refresh_token,
};

struct NotificationDormantState {};

struct NotificationAcquiringState {
    NotificationTimePoint deadline;
    std::size_t failure_count;
};

struct NotificationRefreshingTokenState {
    NotificationTimePoint deadline;
    std::size_t failure_count;
};

struct NotificationConnectingState {
    NotificationTimePoint deadline;
    NotificationTimePoint renew_at;
    std::size_t failure_count;
};

struct NotificationListeningState {
    NotificationTimePoint renew_at;
};

struct NotificationBackoffState {
    NotificationTimePoint retry_at;
    std::size_t failure_count;
    NotificationRetryTarget target;
};

struct NotificationStoppedState {};

using NotificationState = std::variant<
    NotificationDormantState,
    NotificationAcquiringState,
    NotificationRefreshingTokenState,
    NotificationConnectingState,
    NotificationListeningState,
    NotificationBackoffState,
    NotificationStoppedState>;

struct NotificationTransition {
    NotificationState state;
    NotificationCommand command{NotificationCommand::none};
    bool synchronize{false};
};

struct NotificationStartEvent {
    NotificationTimePoint now;
};

struct NotificationChannelAcquiredEvent {
    NotificationTimePoint acquired_at;
    NotificationTimePoint renew_at;
};

struct NotificationChannelFailedEvent {
    NotificationTimePoint failed_at;
    NotificationFailure failure;
};

struct NotificationTokenRefreshedEvent {
    NotificationTimePoint refreshed_at;
};

struct NotificationTokenRefreshFailedEvent {
    NotificationTimePoint failed_at;
};

struct NotificationSocketConnectedEvent {};

struct NotificationSocketDisconnectedEvent {
    NotificationTimePoint disconnected_at;
};

struct RemoteNotificationReceivedEvent {};

struct NotificationDeadlineReachedEvent {
    NotificationTimePoint now;
};

struct NotificationStopEvent {};

using NotificationEvent = std::variant<
    NotificationStartEvent,
    NotificationChannelAcquiredEvent,
    NotificationChannelFailedEvent,
    NotificationTokenRefreshedEvent,
    NotificationTokenRefreshFailedEvent,
    NotificationSocketConnectedEvent,
    NotificationSocketDisconnectedEvent,
    RemoteNotificationReceivedEvent,
    NotificationDeadlineReachedEvent,
    NotificationStopEvent>;

template <typename... Callables> struct NotificationOverloaded : Callables... {
    using Callables::operator()...;
};

[[nodiscard]] inline std::chrono::milliseconds notification_backoff(
    std::size_t failure_count, NotificationTiming timing
) noexcept {
    auto delay = timing.initial_backoff;
    for (std::size_t attempt = 1;
         attempt < failure_count && delay < timing.maximum_backoff;
         ++attempt) {
        delay = delay >= timing.maximum_backoff / 2 ? timing.maximum_backoff
                                                    : delay * 2;
    }
    return std::min(delay, timing.maximum_backoff);
}

[[nodiscard]] inline NotificationTransition notification_backoff_transition(
    NotificationTimePoint failed_at,
    std::size_t previous_failure_count,
    NotificationRetryTarget target,
    NotificationTiming timing
) {
    const auto failure_count = previous_failure_count + 1;
    return {
        .state = NotificationBackoffState{
            .retry_at = failed_at + notification_backoff(failure_count, timing),
            .failure_count = failure_count,
            .target = target,
        },
    };
}

[[nodiscard]] inline NotificationTransition transition_notification(
    NotificationState state,
    const NotificationEvent& event,
    NotificationTiming timing
) {
    return std::visit(
        NotificationOverloaded{
            [](auto&& current,
               const NotificationStopEvent&) -> NotificationTransition {
                using State = std::remove_cvref_t<decltype(current)>;
                constexpr bool has_socket =
                    std::is_same_v<State, NotificationConnectingState> ||
                    std::is_same_v<State, NotificationListeningState>;
                return {
                    .state = NotificationStoppedState{},
                    .command = has_socket
                                   ? NotificationCommand::disconnect_socket
                                   : NotificationCommand::none,
                };
            },
            [&](NotificationDormantState,
                const NotificationStartEvent& start) -> NotificationTransition {
                return {
                    .state =
                        NotificationAcquiringState{
                            .deadline = start.now + timing.request_timeout,
                            .failure_count = 0,
                        },
                    .command = NotificationCommand::acquire_channel,
                };
            },
            [&](NotificationAcquiringState current,
                const NotificationChannelAcquiredEvent& acquired
            ) -> NotificationTransition {
                return {
                    .state =
                        NotificationConnectingState{
                            .deadline =
                                acquired.acquired_at + timing.connect_timeout,
                            .renew_at = acquired.renew_at,
                            .failure_count = current.failure_count,
                        },
                    .command = NotificationCommand::connect_socket,
                };
            },
            [&](NotificationAcquiringState current,
                const NotificationChannelFailedEvent& failed
            ) -> NotificationTransition {
                if (failed.failure == NotificationFailure::unauthorized) {
                    return {
                        .state =
                            NotificationRefreshingTokenState{
                                .deadline =
                                    failed.failed_at + timing.request_timeout,
                                .failure_count = current.failure_count,
                            },
                        .command = NotificationCommand::refresh_token,
                    };
                }
                return notification_backoff_transition(
                    failed.failed_at,
                    current.failure_count,
                    NotificationRetryTarget::acquire_channel,
                    timing
                );
            },
            [&](NotificationRefreshingTokenState current,
                const NotificationTokenRefreshedEvent& refreshed
            ) -> NotificationTransition {
                return {
                    .state =
                        NotificationAcquiringState{
                            .deadline =
                                refreshed.refreshed_at + timing.request_timeout,
                            .failure_count = current.failure_count,
                        },
                    .command = NotificationCommand::acquire_channel,
                };
            },
            [&](NotificationRefreshingTokenState current,
                const NotificationTokenRefreshFailedEvent& failed
            ) -> NotificationTransition {
                return notification_backoff_transition(
                    failed.failed_at,
                    current.failure_count,
                    NotificationRetryTarget::refresh_token,
                    timing
                );
            },
            [](NotificationConnectingState current,
               const NotificationSocketConnectedEvent&
            ) -> NotificationTransition {
                return {
                    .state =
                        NotificationListeningState{
                            .renew_at = current.renew_at,
                        },
                    .synchronize = true,
                };
            },
            [&](NotificationConnectingState current,
                const NotificationSocketDisconnectedEvent& disconnected
            ) -> NotificationTransition {
                return notification_backoff_transition(
                    disconnected.disconnected_at,
                    current.failure_count,
                    NotificationRetryTarget::acquire_channel,
                    timing
                );
            },
            [](NotificationListeningState current,
               const RemoteNotificationReceivedEvent&
            ) -> NotificationTransition {
                return {
                    .state = current,
                    .synchronize = true,
                };
            },
            [&](NotificationListeningState,
                const NotificationSocketDisconnectedEvent& disconnected
            ) -> NotificationTransition {
                return notification_backoff_transition(
                    disconnected.disconnected_at,
                    0,
                    NotificationRetryTarget::acquire_channel,
                    timing
                );
            },
            [&](NotificationListeningState current,
                const NotificationDeadlineReachedEvent& deadline
            ) -> NotificationTransition {
                if (deadline.now < current.renew_at) {
                    return {.state = current};
                }
                return {
                    .state =
                        NotificationAcquiringState{
                            .deadline = deadline.now + timing.request_timeout,
                            .failure_count = 0,
                        },
                    .command = NotificationCommand::renew_channel,
                };
            },
            [&](NotificationBackoffState current,
                const NotificationDeadlineReachedEvent& deadline
            ) -> NotificationTransition {
                if (deadline.now < current.retry_at) {
                    return {.state = current};
                }
                if (current.target == NotificationRetryTarget::refresh_token) {
                    return {
                        .state =
                            NotificationRefreshingTokenState{
                                .deadline =
                                    deadline.now + timing.request_timeout,
                                .failure_count = current.failure_count,
                            },
                        .command = NotificationCommand::refresh_token,
                    };
                }
                return {
                    .state =
                        NotificationAcquiringState{
                            .deadline = deadline.now + timing.request_timeout,
                            .failure_count = current.failure_count,
                        },
                    .command = NotificationCommand::acquire_channel,
                };
            },
            [&](NotificationAcquiringState current,
                const NotificationDeadlineReachedEvent& deadline
            ) -> NotificationTransition {
                if (deadline.now < current.deadline) {
                    return {.state = current};
                }
                return notification_backoff_transition(
                    deadline.now,
                    current.failure_count,
                    NotificationRetryTarget::acquire_channel,
                    timing
                );
            },
            [&](NotificationRefreshingTokenState current,
                const NotificationDeadlineReachedEvent& deadline
            ) -> NotificationTransition {
                if (deadline.now < current.deadline) {
                    return {.state = current};
                }
                return notification_backoff_transition(
                    deadline.now,
                    current.failure_count,
                    NotificationRetryTarget::refresh_token,
                    timing
                );
            },
            [&](NotificationConnectingState current,
                const NotificationDeadlineReachedEvent& deadline
            ) -> NotificationTransition {
                if (deadline.now < current.deadline) {
                    return {.state = current};
                }
                auto transition = notification_backoff_transition(
                    deadline.now,
                    current.failure_count,
                    NotificationRetryTarget::acquire_channel,
                    timing
                );
                transition.command = NotificationCommand::disconnect_socket;
                return transition;
            },
            [](auto&& current, const auto&) -> NotificationTransition {
                return {
                    .state = std::forward<decltype(current)>(current),
                };
            },
        },
        std::move(state),
        event
    );
}

[[nodiscard]] inline std::optional<NotificationTimePoint>
next_notification_deadline(const NotificationState& state) noexcept {
    if (const auto* acquiring =
            std::get_if<NotificationAcquiringState>(&state)) {
        return acquiring->deadline;
    }
    if (const auto* refreshing =
            std::get_if<NotificationRefreshingTokenState>(&state)) {
        return refreshing->deadline;
    }
    if (const auto* connecting =
            std::get_if<NotificationConnectingState>(&state)) {
        return connecting->deadline;
    }
    if (const auto* listening =
            std::get_if<NotificationListeningState>(&state)) {
        return listening->renew_at;
    }
    if (const auto* backoff = std::get_if<NotificationBackoffState>(&state)) {
        return backoff->retry_at;
    }
    return std::nullopt;
}

} // namespace onedrive::monitor::detail
