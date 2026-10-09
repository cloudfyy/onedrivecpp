#include "onedrive/app/authentication.hpp"

#include "onedrive/app/factory.hpp"
#include "onedrive/app/options.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "operation_state.hpp"

#include <stdexcept>

namespace onedrive::app {
namespace {

auto cancelled() {
    return std::unexpected(
        auth::AuthError{
            .code = auth::AuthErrorCode::cancelled,
            .message = "device authorization cancelled",
        }
    );
}

class NullObserver final : public events::Observer {
private:
    void on_event(const events::Event&) const override {}
};

auth::AuthResult<AuthenticationResult> authenticate_account_impl(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const AuthorizationCallback& authorization,
    std::stop_token stop_token
) {
    if (stop_token.stop_requested()) {
        return cancelled();
    }
    auto transport = runtime_factory.create_http_transport(config);
    auto client = runtime_factory.create_device_auth_client(config, *transport);
    const auto code = client->request_device_code(stop_token);
    if (!code) {
        return std::unexpected(code.error());
    }
    if (stop_token.stop_requested()) {
        return cancelled();
    }
    authorization({
        .user_code = code->user_code,
        .verification_uri = code->verification_uri,
        .message = code->message,
        .expires_in = code->expires_in,
    });
    const auto tokens = client->poll_for_token(*code, stop_token);
    if (!tokens) {
        return std::unexpected(tokens.error());
    }
    if (stop_token.stop_requested()) {
        return cancelled();
    }
    account::DriveIdentity identity;
    try {
        identity = graph::fetch_drive_identity(
            *transport, tokens->access_token, graph_options(config), stop_token
        );
    } catch (const std::runtime_error&) {
        if (stop_token.stop_requested()) {
            return cancelled();
        }
        throw;
    }
    if (stop_token.stop_requested()) {
        return cancelled();
    }
    const auto paths = account::AccountState::activate(
        config.state_directory, identity, tokens->refresh_token
    );
    return AuthenticationResult{
        .identity = std::move(identity),
        .token_directory = paths.token_directory,
    };
}

} // namespace

auth::AuthResult<AuthenticationResult> authenticate_account(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const events::Observer& observer,
    const AuthorizationCallback& authorization,
    std::stop_token stop_token
) {
    if (!authorization) {
        throw std::invalid_argument{
            "authentication requires an authorization callback"
        };
    }
    detail::OperationStateMachine state{
        observer, events::OperationKind::authentication
    };
    state.transition(events::OperationState::authenticating);
    try {
        auto result = authenticate_account_impl(
            config, runtime_factory, authorization, stop_token
        );
        if (result) {
            state.transition(events::OperationState::ready);
        } else if (result.error().code == auth::AuthErrorCode::cancelled) {
            state.transition(events::OperationState::idle);
        } else {
            state.transition(events::OperationState::failed);
        }
        return result;
    } catch (...) {
        state.transition(events::OperationState::failed);
        throw;
    }
}

auth::AuthResult<AuthenticationResult> authenticate_account(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const AuthorizationCallback& authorization,
    std::stop_token stop_token
) {
    static const NullObserver observer;
    return authenticate_account(
        config, runtime_factory, observer, authorization, stop_token
    );
}

} // namespace onedrive::app
