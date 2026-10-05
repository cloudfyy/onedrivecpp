#pragma once

#include <concepts>
#include <functional>
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

template <
    typename Next,
    typename Current,
    typename Family,
    typename Payload,
    typename Mapper>
    requires TransactionStateFor<Next, Family> &&
             TransactionStateFor<Current, Family> &&
             std::invocable<Mapper, Payload&&> &&
             (!std::is_void_v<std::invoke_result_t<Mapper, Payload &&>>) &&
             (!std::is_reference_v<std::invoke_result_t<Mapper, Payload &&>>)
[[nodiscard]] auto transition_transaction(
    StateTransaction<Current, Family, Payload>&& transaction, Mapper&& mapper
) noexcept(std::is_nothrow_invocable_v<Mapper, Payload&&>) {
    using NextPayload =
        std::remove_cv_t<std::invoke_result_t<Mapper, Payload&&>>;
    return StateTransaction<Next, Family, NextPayload>{
        std::invoke(
            std::forward<Mapper>(mapper),
            std::move(static_cast<Payload&>(transaction))
        ),
    };
}

} // namespace onedrive::sync::detail
