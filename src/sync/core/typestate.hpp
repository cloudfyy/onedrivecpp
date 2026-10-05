#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

namespace onedrive::sync::detail {

template <typename Family> struct TransactionState {};

template <typename State, typename Family>
concept TransactionStateFor =
    std::derived_from<State, TransactionState<Family>>;

template <typename State, typename Family, typename Payload>
    requires TransactionStateFor<State, Family>
struct StateTransaction : Payload {
    using state_type = State;
    using family_type = Family;
    using payload_type = Payload;
};

template <typename Next, typename Current, typename Family, typename Payload>
    requires TransactionStateFor<Next, Family> &&
             TransactionStateFor<Current, Family>
[[nodiscard]] StateTransaction<Next, Family, Payload> transition_transaction(
    StateTransaction<Current, Family, Payload>&& transaction
) noexcept(std::is_nothrow_move_constructible_v<Payload>) {
    return {
        std::move(static_cast<Payload&>(transaction)),
    };
}

} // namespace onedrive::sync::detail
