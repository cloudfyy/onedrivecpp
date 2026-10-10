#include "auth/code_transaction.hpp"
#include "support/common.hpp"

#include <algorithm>
#include <array>
#include <tuple>

namespace {

using namespace onedrive;
using namespace auth::detail;

template <typename Transaction, typename Next>
concept CanAdvance = requires(Transaction transaction) {
    util::transition_transaction<Next>(std::move(transaction));
};
template <typename Transaction>
concept HasCsrf = requires(Transaction transaction) { transaction.csrf; };
template <typename Transaction>
concept HasCode = requires(Transaction transaction) { transaction.code; };

static_assert(CanAdvance<CreatedCode, AwaitingCallback>);
static_assert(CanAdvance<AwaitingCode, Authorized>);
static_assert(CanAdvance<AuthorizedCode, ExchangingToken>);
static_assert(CanAdvance<ExchangingCode, Completed>);
static_assert(!CanAdvance<CreatedCode, Authorized>);
static_assert(!CanAdvance<CreatedCode, Completed>);
static_assert(!CanAdvance<AwaitingCode, ExchangingToken>);
static_assert(!CanAdvance<AuthorizedCode, AwaitingCallback>);
static_assert(!CanAdvance<ExchangingCode, Authorized>);
static_assert(!CanAdvance<CompletedCode, Created>);
static_assert(!CanAdvance<CancelledCode, Created>);
static_assert(!CanAdvance<ExpiredCode, AwaitingCallback>);
static_assert(!CanAdvance<FailedCode, Authorized>);
static_assert(!CanAdvance<AwaitingCode, AwaitingCallback>);
static_assert(HasCsrf<AwaitingCode> && !HasCode<AwaitingCode>);
static_assert(!HasCsrf<AuthorizedCode> && HasCode<AuthorizedCode>);
static_assert(!HasCsrf<ExchangingCode> && HasCode<ExchangingCode>);
static_assert(!HasCode<CompletedCode> && !HasCsrf<FailedCode>);
static_assert(!std::copy_constructible<AwaitingCode>);
static_assert(std::is_nothrow_move_constructible_v<AwaitingCode>);

using Transactions = std::tuple<
    CreatedCode,
    AwaitingCode,
    AuthorizedCode,
    ExchangingCode,
    CompletedCode,
    CancelledCode,
    ExpiredCode,
    FailedCode>;
template <std::size_t Current, std::size_t... Next>
consteval bool legal_row(std::index_sequence<Next...>) {
    return (
        (CanAdvance<
             std::tuple_element_t<Current, Transactions>,
             typename std::tuple_element_t<Next, Transactions>::state_type> ==
         ((Current < 4 && Next >= 5) ||
          (Current < 4 && Next == Current + 1))) &&
        ...
    );
}
template <std::size_t... Current>
consteval bool legal_matrix(std::index_sequence<Current...>) {
    return (legal_row<Current>(std::make_index_sequence<8>{}) && ...);
}
static_assert(legal_matrix(std::make_index_sequence<8>{}));
struct UnrelatedFamily;
struct UnrelatedState : util::TransactionState<UnrelatedFamily> {};
static_assert(!CanAdvance<CreatedCode, UnrelatedState>);

struct CleanseRecord {
    const void* pointer;
    std::size_t size;
    bool zeroed;
};
std::array<CleanseRecord, 512> records{};
std::size_t record_count{0};

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error{message};
}

bool cleaned(const char* pointer, std::size_t size) {
    return std::any_of(
        records.begin(),
        records.begin() + record_count,
        [&](const auto& record) {
            return record.pointer == pointer && record.size == size &&
                   record.zeroed;
        }
    );
}

AwaitingCode waiting() {
    return util::transition_transaction<AwaitingCallback>(
        CreatedCode{}, [](EmptyCodePayload&&) {
            return CallbackPayload{
                "http://localhost/",
                {},
                CodeSecret{std::string(43, 'v')},
                CodeSecret{std::string(43, 's')}
            };
        }
    );
}

void test_owned_moves_and_terminal_cleanup() {
    CodeStorage storage{waiting()};
    const auto* verifier =
        std::get<AwaitingCode>(storage).verifier.get().data();
    const auto* csrf = std::get<AwaitingCode>(storage).csrf.get().data();
    CodeSecret code{"x"};
    const auto* code_pointer = code.get().data();
    record_count = 0;
    storage = util::transition_transaction<Authorized>(
        std::move(std::get<AwaitingCode>(storage)),
        [&](CallbackPayload&& payload) {
            return AuthorizedPayload{
                std::move(payload.redirect),
                payload.deadline,
                std::move(payload.verifier),
                std::move(code)
            };
        }
    );
    require(
        cleaned(csrf, 43) &&
            std::get<AuthorizedCode>(storage).verifier.get().data() ==
                verifier &&
            std::get<AuthorizedCode>(storage).code.get().data() ==
                code_pointer &&
            !cleaned(verifier, 43) && !cleaned(code_pointer, 1),
        "authorization copied secrets or failed to cleanse consumed CSRF"
    );
    storage = util::transition_transaction<ExchangingToken>(
        std::move(std::get<AuthorizedCode>(storage)),
        [](AuthorizedPayload&& payload) {
            return ExchangePayload{
                std::move(payload.redirect),
                std::move(payload.verifier),
                std::move(payload.code)
            };
        }
    );
    require(
        std::get<ExchangingCode>(storage).verifier.get().data() == verifier &&
            std::get<ExchangingCode>(storage).code.get().data() == code_pointer,
        "exchange moved string storage instead of its owner"
    );
    storage = util::transition_transaction<Completed>(
        std::move(std::get<ExchangingCode>(storage)),
        [](ExchangePayload&&) { return EmptyCodePayload{}; }
    );
    require(
        cleaned(verifier, 43) && cleaned(code_pointer, 1),
        "completed transaction retained verifier or code bytes"
    );
}

template <typename Terminal> void test_terminal() {
    CodeStorage storage{waiting()};
    const auto* verifier =
        std::get<AwaitingCode>(storage).verifier.get().data();
    const auto* csrf = std::get<AwaitingCode>(storage).csrf.get().data();
    record_count = 0;
    storage = util::transition_transaction<Terminal>(
        std::move(std::get<AwaitingCode>(storage)), [](CallbackPayload&&) {
            return CodeErrorPayload{
                {.code = auth::AuthErrorCode::server, .message = "test error"}
            };
        }
    );
    require(
        cleaned(verifier, 43) && cleaned(csrf, 43),
        "terminal transaction did not cleanse its active secrets"
    );
}

void test_unwinding_and_replacement() {
    const char* verifier = nullptr;
    record_count = 0;
    require(
        test::throws_with<std::runtime_error>([&] {
            auto current = waiting();
            verifier = current.verifier.get().data();
            static_cast<void>(util::transition_transaction<Authorized>(
                std::move(current), [](CallbackPayload&&) -> AuthorizedPayload {
                    throw std::runtime_error{"mapper exception"};
                }
            ));
        }),
        "transition swallowed its mapper exception"
    );
    require(
        cleaned(verifier, 43), "unwinding failed to cleanse transaction secrets"
    );
    CodeSecret value{"old"};
    const auto* old = value.get().data();
    record_count = 0;
    value.assign("new");
    require(
        cleaned(old, 3) && value.get() == "new",
        "secret reassignment did not cleanse its old bytes"
    );
    CodeSecret replacement{"replacement"};
    value = std::move(replacement);
    require(
        value.get() == "replacement", "secret owner move assignment failed"
    );
}

} // namespace

extern "C" void __real_OPENSSL_cleanse(void*, std::size_t);
extern "C" void __wrap_OPENSSL_cleanse(void* pointer, std::size_t size) {
    __real_OPENSSL_cleanse(pointer, size);
    if (size != 0 && record_count < records.size()) {
        const auto* bytes = static_cast<const unsigned char*>(pointer);
        records[record_count++] = {
            pointer, size, std::all_of(bytes, bytes + size, [](auto byte) {
                return byte == 0;
            })
        };
    }
}

int main() {
    try {
        test_owned_moves_and_terminal_cleanup();
        test_terminal<Cancelled>();
        test_terminal<Expired>();
        test_terminal<Failed>();
        test_unwinding_and_replacement();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        return test::fail(error.what());
    }
}
