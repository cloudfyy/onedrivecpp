#include "support.hpp"

namespace {

using namespace onedrive::test::monitor;

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
    auto ignored =
        transition_monitor(state, DeadlineReachedEvent{origin}, timing);
    if (ignored.effect != MonitorEffect::none ||
        !std::holds_alternative<StartingState>(ignored.state)) {
        return fail("starting monitor accepted an invalid deadline event");
    }

    auto transition =
        transition_monitor(std::move(state), StartEvent{}, timing);
    if (transition.effect != MonitorEffect::synchronize ||
        std::get<SynchronizingState>(transition.state).reason !=
            SynchronizationReason::initial) {
        return fail("monitor did not start with initial synchronization");
    }
    ignored = transition_monitor(
        transition.state, DeadlineReachedEvent{origin}, timing
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
    )
                .state;
    const auto* idle = std::get_if<IdleState>(&state);
    if (idle == nullptr || idle->graph_deadline != origin + 110ms ||
        next_deadline(state) != idle->graph_deadline) {
        return fail("monitor did not schedule its first Graph poll");
    }

    transition =
        transition_monitor(state, DeadlineReachedEvent{origin + 109ms}, timing);
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
    if (settling == nullptr || settling->local_deadline != origin + 35ms ||
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
    if (settling == nullptr || settling->local_deadline != origin + 40ms) {
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
    if (settling == nullptr || settling->local_deadline != origin + 50ms ||
        settling->reason != SynchronizationReason::watch_overflow ||
        next_deadline(transition.state) != origin + 50ms) {
        return fail("monitor did not preserve an overflow while settling");
    }

    transition = transition_monitor(
        std::move(transition.state), DeadlineReachedEvent{origin + 49ms}, timing
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
    )
                .state;
    if (std::get<SynchronizingState>(state).reason !=
        SynchronizationReason::graph_poll) {
        return fail("Graph deadline did not trigger synchronization");
    }
    transition = transition_monitor(state, RemoteChangeEvent{}, timing);
    const auto* synchronizing =
        std::get_if<SynchronizingState>(&transition.state);
    if (transition.effect != MonitorEffect::none || synchronizing == nullptr ||
        synchronizing->reason != SynchronizationReason::graph_poll ||
        !synchronizing->remote_notification_pending) {
        return fail(
            "remote notification was not latched during synchronization"
        );
    }
    transition = transition_monitor(
        std::move(transition.state),
        SynchronizationCompletedEvent{
            .status = 0,
            .completed_at = origin + 65ms,
        },
        timing
    );
    if (transition.effect != MonitorEffect::synchronize ||
        std::get<SynchronizingState>(transition.state).reason !=
            SynchronizationReason::remote_notification) {
        return fail("latched remote notification did not trigger a catch-up");
    }
    state = transition_monitor(
                std::move(transition.state),
                SynchronizationCompletedEvent{
                    .status = 2,
                    .completed_at = origin + 70ms,
                },
                timing
    )
                .state;
    if (std::get<IdleState>(state).graph_deadline != origin + 170ms) {
        return fail("completed synchronization did not reset Graph polling");
    }
    transition = transition_monitor(state, RemoteChangeEvent{}, timing);
    if (transition.effect != MonitorEffect::synchronize ||
        std::get<SynchronizingState>(transition.state).reason !=
            SynchronizationReason::remote_notification) {
        return fail("remote notification did not trigger synchronization");
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
        StoppedState{}, DeadlineReachedEvent{origin}, timing
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
            std::move(stoppable), StopRequestedEvent{}, timing
        );
        if (transition.effect != MonitorEffect::stop ||
            !std::holds_alternative<StoppedState>(transition.state) ||
            next_deadline(transition.state)) {
            return fail("monitor state did not stop cleanly");
        }
    }

    if (std::string_view{
            synchronization_reason_name(SynchronizationReason::initial)
        }
            .empty() ||
        std::string_view{
            synchronization_reason_name(SynchronizationReason::local_changes)
        }
            .empty() ||
        std::string_view{
            synchronization_reason_name(SynchronizationReason::watch_overflow)
        }
            .empty() ||
        std::string_view{synchronization_reason_name(
                             SynchronizationReason::remote_notification
                         )}
            .empty() ||
        std::string_view{
            synchronization_reason_name(SynchronizationReason::graph_poll)
        }
            .empty()) {
        return fail("monitor synchronization reason was not printable");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_state_machine();
}
