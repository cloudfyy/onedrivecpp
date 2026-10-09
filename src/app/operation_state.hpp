#pragma once

#include "onedrive/ui/common/observer.hpp"

#include <stdexcept>

namespace onedrive::app::detail {

class OperationStateMachine final {
public:
    OperationStateMachine(
        const events::Observer& observer, events::OperationKind operation
    )
        : observer_{observer}, operation_{operation} {}

    void transition(events::OperationState next) {
        using events::OperationKind;
        using events::OperationState;

        const bool allowed =
            (state_ == OperationState::idle &&
             ((operation_ == OperationKind::authentication &&
               next == OperationState::authenticating) ||
              (operation_ == OperationKind::synchronization &&
               next == OperationState::syncing) ||
              (operation_ == OperationKind::monitoring &&
               next == OperationState::watching))) ||
            (state_ == OperationState::authenticating &&
             (next == OperationState::ready || next == OperationState::idle ||
              next == OperationState::failed)) ||
            (state_ == OperationState::syncing &&
             (next == OperationState::ready ||
              next == OperationState::stopping ||
              next == OperationState::failed)) ||
            (state_ == OperationState::watching &&
             (next == OperationState::ready ||
              next == OperationState::stopping ||
              next == OperationState::failed)) ||
            (state_ == OperationState::stopping &&
             (next == OperationState::ready ||
              next == OperationState::failed));
        if (!allowed) {
            throw std::logic_error{"invalid application operation state transition"};
        }
        observer_.operation_state(operation_, next);
        state_ = next;
    }

private:
    const events::Observer& observer_;
    events::OperationKind operation_;
    events::OperationState state_{events::OperationState::idle};
};

}  // namespace onedrive::app::detail
