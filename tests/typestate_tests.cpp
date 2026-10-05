#include "sync/core/typestate.hpp"

#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>

namespace {

struct TransferFamily;
using TransferState = onedrive::sync::detail::TransactionState<TransferFamily>;
struct PreparedState final : TransferState {};
struct JournaledState final : TransferState {};

struct OtherFamily;
using OtherState = onedrive::sync::detail::TransactionState<OtherFamily>;
struct UnrelatedState final : OtherState {};

struct MoveOnlyPayload {
    std::unique_ptr<int> value;
};

using PreparedTransaction = onedrive::sync::detail::
    StateTransaction<PreparedState, TransferFamily, MoveOnlyPayload>;
using JournaledTransaction = onedrive::sync::detail::
    StateTransaction<JournaledState, TransferFamily, MoveOnlyPayload>;

static_assert(
    onedrive::sync::detail::TransactionStateFor<PreparedState, TransferFamily>
);
static_assert(
    !onedrive::sync::detail::TransactionStateFor<UnrelatedState, TransferFamily>
);
static_assert(!std::copyable<PreparedTransaction>);
static_assert(std::is_nothrow_move_constructible_v<PreparedTransaction>);
static_assert(noexcept(onedrive::sync::detail::transition_transaction<
                       JournaledState>(std::declval<PreparedTransaction&&>())));
static_assert(!std::same_as<PreparedTransaction, JournaledTransaction>);

} // namespace

int main() {
    auto prepared = PreparedTransaction{
        MoveOnlyPayload{std::make_unique<int>(42)},
    };
    auto journaled =
        onedrive::sync::detail::transition_transaction<JournaledState>(
            std::move(prepared)
        );
    return journaled.value && *journaled.value == 42 ? EXIT_SUCCESS
                                                     : EXIT_FAILURE;
}
