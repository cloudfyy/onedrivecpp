#include "monitor/notify.hpp"
#include "support/common.hpp"

#include <chrono>
#include <cstdlib>

namespace {

using namespace std::chrono_literals;
using namespace onedrive::monitor::detail;
using onedrive::test::fail;

constexpr NotificationTiming timing{
    .request_timeout = 30s,
    .connect_timeout = 10s,
    .initial_backoff = 1s,
    .maximum_backoff = 8s,
};

int test_connecting_state_ignored_event() {
    const NotificationTimePoint origin{};
    const std::string url =
        "https://notify.example.test/channel-with-owned-notification-url";
    const NotificationState original = NotificationConnectingState{
        .deadline = origin + 10s,
        .renew_at = origin + 1h,
        .failure_count = 3,
        .notification_url = url,
    };
    const auto transition = transition_notification(
        original, RemoteNotificationReceivedEvent{}, timing
    );
    const auto* connecting =
        std::get_if<NotificationConnectingState>(&transition.state);
    if (connecting == nullptr || connecting->notification_url != url ||
        connecting->deadline != origin + 10s ||
        connecting->renew_at != origin + 1h || connecting->failure_count != 3 ||
        transition.command != NotificationCommand::none ||
        transition.synchronize ||
        std::get<NotificationConnectingState>(original).notification_url !=
            url) {
        return fail(
            "ignored notification event lost the owned connecting state"
        );
    }
    return EXIT_SUCCESS;
}

int test_happy_path_and_renewal() {
    const NotificationTimePoint origin{};
    NotificationState state = NotificationDormantState{};

    auto transition =
        transition_notification(state, NotificationStartEvent{origin}, timing);
    auto* acquiring =
        std::get_if<NotificationAcquiringState>(&transition.state);
    if (transition.command != NotificationCommand::acquire_channel ||
        transition.synchronize || acquiring == nullptr ||
        acquiring->deadline != origin + 30s ||
        next_notification_deadline(transition.state) != origin + 30s) {
        return fail("notification channel acquisition did not start");
    }

    transition = transition_notification(
        std::move(transition.state),
        NotificationChannelAcquiredEvent{
            .acquired_at = origin + 1s,
            .renew_at = origin + 1h,
            .notification_url = "https://notify.example.test/channel",
        },
        timing
    );
    const auto* connecting =
        std::get_if<NotificationConnectingState>(&transition.state);
    if (transition.command != NotificationCommand::connect_socket ||
        connecting == nullptr || connecting->deadline != origin + 11s ||
        connecting->renew_at != origin + 1h ||
        connecting->notification_url != "https://notify.example.test/channel") {
        return fail("acquired notification channel did not begin connecting");
    }

    transition = transition_notification(
        std::move(transition.state), NotificationSocketConnectedEvent{}, timing
    );
    if (transition.command != NotificationCommand::none ||
        !transition.synchronize ||
        !std::holds_alternative<NotificationListeningState>(transition.state)) {
        return fail(
            "connected socket did not request catch-up synchronization"
        );
    }

    transition = transition_notification(
        std::move(transition.state), RemoteNotificationReceivedEvent{}, timing
    );
    if (!transition.synchronize ||
        !std::holds_alternative<NotificationListeningState>(transition.state)) {
        return fail("remote notification did not request synchronization");
    }

    auto early = transition_notification(
        transition.state,
        NotificationDeadlineReachedEvent{origin + 59min},
        timing
    );
    if (early.command != NotificationCommand::none ||
        !std::holds_alternative<NotificationListeningState>(early.state)) {
        return fail("notification channel renewed before its deadline");
    }

    transition = transition_notification(
        std::move(transition.state),
        NotificationDeadlineReachedEvent{origin + 1h},
        timing
    );
    acquiring = std::get_if<NotificationAcquiringState>(&transition.state);
    if (transition.command != NotificationCommand::renew_channel ||
        acquiring == nullptr || acquiring->deadline != origin + 1h + 30s) {
        return fail("notification channel did not renew at its deadline");
    }
    return EXIT_SUCCESS;
}

int test_authentication_recovery() {
    const NotificationTimePoint origin{};
    auto transition = transition_notification(
        NotificationAcquiringState{origin + 30s, 0},
        NotificationChannelFailedEvent{
            .failed_at = origin,
            .failure = NotificationFailure::unauthorized,
        },
        timing
    );
    auto* refreshing =
        std::get_if<NotificationRefreshingTokenState>(&transition.state);
    if (transition.command != NotificationCommand::refresh_token ||
        refreshing == nullptr || refreshing->deadline != origin + 30s) {
        return fail("unauthorized channel request did not refresh its token");
    }

    transition = transition_notification(
        std::move(transition.state),
        NotificationTokenRefreshFailedEvent{origin + 1s},
        timing
    );
    auto* backoff = std::get_if<NotificationBackoffState>(&transition.state);
    if (backoff == nullptr || backoff->retry_at != origin + 2s ||
        backoff->failure_count != 1 ||
        backoff->target != NotificationRetryTarget::refresh_token) {
        return fail("failed token refresh did not enter retry backoff");
    }

    auto early = transition_notification(
        transition.state,
        NotificationDeadlineReachedEvent{origin + 1500ms},
        timing
    );
    if (early.command != NotificationCommand::none ||
        !std::holds_alternative<NotificationBackoffState>(early.state)) {
        return fail("token refresh retried before its backoff deadline");
    }

    transition = transition_notification(
        std::move(transition.state),
        NotificationDeadlineReachedEvent{origin + 2s},
        timing
    );
    if (transition.command != NotificationCommand::refresh_token ||
        !std::holds_alternative<NotificationRefreshingTokenState>(
            transition.state
        )) {
        return fail("token refresh did not retry after backoff");
    }

    transition = transition_notification(
        std::move(transition.state),
        NotificationTokenRefreshedEvent{origin + 3s},
        timing
    );
    if (transition.command != NotificationCommand::acquire_channel ||
        !std::holds_alternative<NotificationAcquiringState>(transition.state)) {
        return fail("refreshed token did not retry channel acquisition");
    }
    return EXIT_SUCCESS;
}

int test_transient_retry_and_reconnect() {
    const NotificationTimePoint origin{};
    NotificationState state = NotificationAcquiringState{origin + 30s, 0};
    for (std::size_t failure = 1; failure <= 5; ++failure) {
        auto transition = transition_notification(
            std::move(state),
            NotificationChannelFailedEvent{
                .failed_at = origin,
                .failure = NotificationFailure::transient,
            },
            timing
        );
        const auto* backoff =
            std::get_if<NotificationBackoffState>(&transition.state);
        const auto expected_delay =
            failure < 4 ? 1s * (1 << (failure - 1)) : 8s;
        if (backoff == nullptr ||
            backoff->retry_at != origin + expected_delay ||
            backoff->failure_count != failure) {
            return fail("transient retry backoff was not bounded exponential");
        }
        const auto retry_at = backoff->retry_at;
        transition = transition_notification(
            std::move(transition.state),
            NotificationDeadlineReachedEvent{retry_at},
            timing
        );
        if (transition.command != NotificationCommand::acquire_channel) {
            return fail("channel acquisition did not retry after backoff");
        }
        state = std::move(transition.state);
    }

    auto transition = transition_notification(
        NotificationListeningState{origin + 1h},
        NotificationSocketDisconnectedEvent{origin + 10s},
        timing
    );
    auto* backoff = std::get_if<NotificationBackoffState>(&transition.state);
    if (backoff == nullptr || backoff->retry_at != origin + 11s ||
        backoff->target != NotificationRetryTarget::acquire_channel) {
        return fail("disconnected socket did not reacquire its channel");
    }
    return EXIT_SUCCESS;
}

int test_timeouts_and_stop() {
    const NotificationTimePoint origin{};
    auto early = transition_notification(
        NotificationAcquiringState{origin + 10s, 0},
        NotificationDeadlineReachedEvent{origin + 9s},
        timing
    );
    if (!std::holds_alternative<NotificationAcquiringState>(early.state)) {
        return fail("channel acquisition timed out before its deadline");
    }

    auto transition = transition_notification(
        NotificationAcquiringState{origin + 10s, 0},
        NotificationDeadlineReachedEvent{origin + 10s},
        timing
    );
    if (!std::holds_alternative<NotificationBackoffState>(transition.state)) {
        return fail("channel acquisition timeout did not enter backoff");
    }

    early = transition_notification(
        NotificationRefreshingTokenState{origin + 10s, 0},
        NotificationDeadlineReachedEvent{origin + 9s},
        timing
    );
    if (!std::holds_alternative<NotificationRefreshingTokenState>(early.state
        )) {
        return fail("token refresh timed out before its deadline");
    }

    transition = transition_notification(
        NotificationRefreshingTokenState{origin + 10s, 0},
        NotificationDeadlineReachedEvent{origin + 10s},
        timing
    );
    auto* backoff = std::get_if<NotificationBackoffState>(&transition.state);
    if (backoff == nullptr ||
        backoff->target != NotificationRetryTarget::refresh_token) {
        return fail("token refresh timeout did not retry token refresh");
    }

    early = transition_notification(
        NotificationConnectingState{
            .deadline = origin + 10s,
            .renew_at = origin + 1h,
            .failure_count = 0,
        },
        NotificationDeadlineReachedEvent{origin + 9s},
        timing
    );
    if (!std::holds_alternative<NotificationConnectingState>(early.state)) {
        return fail("socket connection timed out before its deadline");
    }

    transition = transition_notification(
        NotificationConnectingState{
            .deadline = origin + 10s,
            .renew_at = origin + 1h,
            .failure_count = 0,
        },
        NotificationDeadlineReachedEvent{origin + 10s},
        timing
    );
    if (transition.command != NotificationCommand::disconnect_socket ||
        !std::holds_alternative<NotificationBackoffState>(transition.state)) {
        return fail("socket connection timeout did not disconnect and retry");
    }

    transition = transition_notification(
        NotificationConnectingState{
            .deadline = origin + 10s,
            .renew_at = origin + 1h,
            .failure_count = 2,
        },
        NotificationSocketDisconnectedEvent{origin + 5s},
        timing
    );
    backoff = std::get_if<NotificationBackoffState>(&transition.state);
    if (backoff == nullptr || backoff->failure_count != 3) {
        return fail("failed socket connection did not preserve retry count");
    }

    transition = transition_notification(
        NotificationDormantState{}, RemoteNotificationReceivedEvent{}, timing
    );
    if (!std::holds_alternative<NotificationDormantState>(transition.state) ||
        transition.synchronize) {
        return fail("irrelevant notification changed dormant state");
    }

    for (const NotificationState& deadline_state : {
             NotificationState{NotificationAcquiringState{origin + 1s, 0}},
             NotificationState{
                 NotificationRefreshingTokenState{origin + 2s, 0}
             },
             NotificationState{NotificationConnectingState{
                 origin + 3s,
                 origin + 1h,
                 0,
             }},
             NotificationState{NotificationListeningState{origin + 4s}},
             NotificationState{NotificationBackoffState{
                 origin + 5s,
                 1,
                 NotificationRetryTarget::acquire_channel,
             }},
         }) {
        if (!next_notification_deadline(deadline_state)) {
            return fail("active notification state had no deadline");
        }
    }

    for (NotificationState stoppable : {
             NotificationState{NotificationDormantState{}},
             NotificationState{NotificationAcquiringState{origin + 30s, 0}},
             NotificationState{NotificationRefreshingTokenState{
                 origin + 30s,
                 0,
             }},
             NotificationState{NotificationConnectingState{
                 origin + 10s,
                 origin + 1h,
                 0,
             }},
             NotificationState{NotificationListeningState{origin + 1h}},
             NotificationState{NotificationBackoffState{
                 origin + 1s,
                 1,
                 NotificationRetryTarget::acquire_channel,
             }},
             NotificationState{NotificationStoppedState{}},
         }) {
        const bool had_socket =
            std::holds_alternative<NotificationConnectingState>(stoppable) ||
            std::holds_alternative<NotificationListeningState>(stoppable);
        transition = transition_notification(
            std::move(stoppable), NotificationStopEvent{}, timing
        );
        const auto expected_command =
            had_socket ? NotificationCommand::disconnect_socket
                       : NotificationCommand::none;
        if (transition.command != expected_command ||
            !std::holds_alternative<NotificationStoppedState>(transition.state
            ) ||
            next_notification_deadline(transition.state)) {
            return fail("notification state did not stop idempotently");
        }
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (const int status = test_connecting_state_ignored_event();
        status != EXIT_SUCCESS) {
        return status;
    }
    if (const int status = test_happy_path_and_renewal();
        status != EXIT_SUCCESS) {
        return status;
    }
    if (const int status = test_authentication_recovery();
        status != EXIT_SUCCESS) {
        return status;
    }
    if (const int status = test_transient_retry_and_reconnect();
        status != EXIT_SUCCESS) {
        return status;
    }
    return test_timeouts_and_stop();
}
