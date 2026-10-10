#include "util/typestate.hpp"

#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>

namespace {

struct TransferFamily;
using TransferState = onedrive::util::TransactionState<TransferFamily>;
struct PreparedState final : TransferState {};
struct JournaledState final : TransferState {};
struct TransferFamily {
    template <typename Current, typename Next>
    [[nodiscard]] static consteval bool allows_transition() {
        return std::same_as<Current, PreparedState> &&
               std::same_as<Next, JournaledState>;
    }
};

struct OtherFamily;
using OtherState = onedrive::util::TransactionState<OtherFamily>;
struct UnrelatedState final : OtherState {};

struct MoveOnlyPayload {
    std::unique_ptr<int> value;
};

struct JournaledPayload {
    int value{0};
};

using PreparedTransaction = onedrive::util::
    StateTransaction<PreparedState, TransferFamily, MoveOnlyPayload>;
using JournaledTransaction = onedrive::util::
    StateTransaction<JournaledState, TransferFamily, JournaledPayload>;

JournaledTransaction
journal_transaction(PreparedTransaction transaction) noexcept {
    return onedrive::util::transition_transaction<JournaledState>(
        std::move(transaction),
        [](MoveOnlyPayload&& payload) noexcept {
            return JournaledPayload{*payload.value};
        }
    );
}

template <typename Transaction>
concept Journalable = requires(Transaction transaction) {
    journal_transaction(std::move(transaction));
};

template <typename Transaction>
concept HasMoveOnlyValue =
    requires(Transaction transaction) { transaction.value.reset(); };

struct ThrowingMapper {
    JournaledPayload operator()(MoveOnlyPayload&& payload) const {
        return {*payload.value};
    }
};

static_assert(onedrive::util::TransactionStateFor<PreparedState, TransferFamily>
);
static_assert(
    !onedrive::util::TransactionStateFor<UnrelatedState, TransferFamily>
);
static_assert(onedrive::util::TransactionTransitionFor<
              PreparedState,
              JournaledState,
              TransferFamily>);
static_assert(!onedrive::util::TransactionTransitionFor<
              JournaledState,
              PreparedState,
              TransferFamily>);
static_assert(!std::copyable<PreparedTransaction>);
static_assert(std::is_nothrow_move_constructible_v<PreparedTransaction>);
static_assert(noexcept(onedrive::util::transition_transaction<JournaledState>(
    std::declval<PreparedTransaction&&>(),
    [](MoveOnlyPayload&& payload) noexcept {
        return JournaledPayload{*payload.value};
    }
)));
static_assert(!noexcept(onedrive::util::transition_transaction<JournaledState>(
    std::declval<PreparedTransaction&&>(), ThrowingMapper{}
)));
static_assert(!std::same_as<PreparedTransaction, JournaledTransaction>);
static_assert(Journalable<PreparedTransaction>);
static_assert(!Journalable<JournaledTransaction>);
static_assert(HasMoveOnlyValue<PreparedTransaction>);
static_assert(!HasMoveOnlyValue<JournaledTransaction>);

} // namespace

int main() {
    auto retained = PreparedTransaction{
        MoveOnlyPayload{std::make_unique<int>(17)},
    };
    const auto* pointer = retained.value.get();
    auto advanced = onedrive::util::transition_transaction<JournaledState>(
        std::move(retained)
    );
    if (retained.value || advanced.value.get() != pointer ||
        *advanced.value != 17)
        return EXIT_FAILURE;
    auto prepared = PreparedTransaction{
        MoveOnlyPayload{std::make_unique<int>(42)},
    };
    auto journaled = journal_transaction(std::move(prepared));
    return journaled.value == 42 ? EXIT_SUCCESS : EXIT_FAILURE;
}
