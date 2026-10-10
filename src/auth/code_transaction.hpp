#pragma once

#include "onedrive/auth/auth_code.hpp"
#include "util/typestate.hpp"

#include <openssl/crypto.h>

#include <memory>
#include <variant>

namespace onedrive::auth::detail {

struct CleanCodeString {
    std::string& value;
    ~CleanCodeString() {
        OPENSSL_cleanse(value.data(), value.size());
    }
};

// Moving the owner never moves a string's small-buffer storage.
class CodeSecret {
public:
    CodeSecret()
        : value_{std::make_unique<std::string>()} {
    }
    explicit CodeSecret(std::string value) {
        const CleanCodeString cleanup{value};
        value_ = std::make_unique<std::string>();
        assign(value);
    }
    CodeSecret(CodeSecret&&) noexcept = default;
    CodeSecret& operator=(CodeSecret&& other) noexcept {
        clear();
        value_ = std::move(other.value_);
        return *this;
    }
    CodeSecret(const CodeSecret&) = delete;
    CodeSecret& operator=(const CodeSecret&) = delete;
    ~CodeSecret() {
        clear();
    }
    [[nodiscard]] const std::string& get() const noexcept {
        return *value_;
    }
    void assign(const std::string& value) {
        clear();
        *value_ = value;
    }

private:
    void clear() noexcept {
        if (value_)
            OPENSSL_cleanse(value_->data(), value_->size());
    }
    std::unique_ptr<std::string> value_;
};

struct CodeFamily;
struct Created : util::TransactionState<CodeFamily> {};
struct AwaitingCallback : util::TransactionState<CodeFamily> {};
struct Authorized : util::TransactionState<CodeFamily> {};
struct ExchangingToken : util::TransactionState<CodeFamily> {};
struct Completed : util::TransactionState<CodeFamily> {};
struct Cancelled : util::TransactionState<CodeFamily> {};
struct Expired : util::TransactionState<CodeFamily> {};
struct Failed : util::TransactionState<CodeFamily> {};

struct CodeFamily {
    template <typename Current, typename Next>
    static consteval bool allows_transition() {
        constexpr bool active = std::same_as<Current, Created> ||
                                std::same_as<Current, AwaitingCallback> ||
                                std::same_as<Current, Authorized> ||
                                std::same_as<Current, ExchangingToken>;
        return (active &&
                (std::same_as<Next, Cancelled> || std::same_as<Next, Expired> ||
                 std::same_as<Next, Failed>)) ||
               (std::same_as<Current, Created> &&
                std::same_as<Next, AwaitingCallback>) ||
               (std::same_as<Current, AwaitingCallback> &&
                std::same_as<Next, Authorized>) ||
               (std::same_as<Current, Authorized> &&
                std::same_as<Next, ExchangingToken>) ||
               (std::same_as<Current, ExchangingToken> &&
                std::same_as<Next, Completed>);
    }
};

struct EmptyCodePayload {};
struct CallbackPayload {
    std::string redirect;
    std::chrono::steady_clock::time_point deadline;
    CodeSecret verifier;
    CodeSecret csrf;
};
struct AuthorizedPayload {
    std::string redirect;
    std::chrono::steady_clock::time_point deadline;
    CodeSecret verifier;
    CodeSecret code;
};
struct ExchangePayload {
    std::string redirect;
    CodeSecret verifier;
    CodeSecret code;
};
struct CodeErrorPayload {
    AuthError error;
};

template <typename State, typename Payload>
using CodeTransaction = util::StateTransaction<State, CodeFamily, Payload>;

using CreatedCode = CodeTransaction<Created, EmptyCodePayload>;
using AwaitingCode = CodeTransaction<AwaitingCallback, CallbackPayload>;
using AuthorizedCode = CodeTransaction<Authorized, AuthorizedPayload>;
using ExchangingCode = CodeTransaction<ExchangingToken, ExchangePayload>;
using CompletedCode = CodeTransaction<Completed, EmptyCodePayload>;
using CancelledCode = CodeTransaction<Cancelled, CodeErrorPayload>;
using ExpiredCode = CodeTransaction<Expired, CodeErrorPayload>;
using FailedCode = CodeTransaction<Failed, CodeErrorPayload>;
using CodeStorage = std::variant<
    CreatedCode,
    AwaitingCode,
    AuthorizedCode,
    ExchangingCode,
    CompletedCode,
    CancelledCode,
    ExpiredCode,
    FailedCode>;

} // namespace onedrive::auth::detail
