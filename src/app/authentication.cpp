#include "onedrive/app/authentication.hpp"

#include "onedrive/app/factory.hpp"
#include "onedrive/app/options.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "operation_state.hpp"
#include "preflight.hpp"

#include <stdexcept>

namespace onedrive::app {
namespace {

auto cancelled() {
    return std::unexpected(
        auth::AuthError{
            .code = auth::AuthErrorCode::cancelled,
            .message = "authentication cancelled",
        }
    );
}

class NullObserver final : public events::Observer {
private:
    void on_event(const events::Event&) const override {
    }
};

template <typename Authenticate>
auth::AuthResult<AuthenticationResult> observe_authentication(
    const events::Observer& observer, Authenticate&& authenticate
) {
    detail::OperationStateMachine state{
        observer, events::OperationKind::authentication
    };
    state.transition(events::OperationState::authenticating);
    try {
        auto result = authenticate();
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

auth::AuthResult<AuthenticationResult> activate_authorized_account(
    const config::Config& config,
    const http::HttpTransport& transport,
    const auth::OAuthTokens& tokens,
    std::stop_token stop_token
) {
    if (stop_token.stop_requested()) {
        return cancelled();
    }
    account::DriveIdentity identity;
    try {
        identity = graph::fetch_drive_identity(
            transport, tokens.access_token, graph_options(config), stop_token
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
        config.state_directory, identity, tokens.refresh_token
    );
    return AuthenticationResult{
        .identity = std::move(identity),
        .token_directory = paths.token_directory,
    };
}

auth::AuthResult<AuthenticationResult> authenticate_device_code_account_impl(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const AuthorizationCallback& authorization,
    std::stop_token stop_token
) {
    if (stop_token.stop_requested()) {
        return cancelled();
    }
    const detail::RuntimePreflight preflight{
        config, detail::Operation::authenticate
    };
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
    return activate_authorized_account(config, *transport, *tokens, stop_token);
}

auth::AuthResult<AuthenticationResult> authenticate_auth_code_account_impl(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const AuthCodeAuthorizationCallback& authorization,
    std::stop_token stop_token
) {
    if (!authorization) {
        throw std::invalid_argument{
            "browser authentication requires an authorization callback"
        };
    }
    if (stop_token.stop_requested()) {
        return cancelled();
    }
    const detail::RuntimePreflight preflight{
        config, detail::Operation::authenticate
    };
    auto transport = runtime_factory.create_http_transport(config);
    auto client = runtime_factory.create_device_auth_client(config, *transport);
    auth::AuthCodeSession session{device_auth_options(config)};
    auth::AuthResult<void> request;
    try {
        request = authorization(session, stop_token);
    } catch (...) {
        session.fail();
        throw;
    }
    if (!request) {
        session.fail(request.error());
        return std::unexpected(request.error());
    }
    const auto tokens = session.exchange(*client, stop_token);
    if (!tokens) {
        return std::unexpected(tokens.error());
    }
    return activate_authorized_account(config, *transport, *tokens, stop_token);
}

} // namespace

auth::AuthResult<AuthenticationResult> authenticate_auth_code_account(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const events::Observer& observer,
    const AuthCodeAuthorizationCallback& authorization,
    std::stop_token stop_token
) {
    return observe_authentication(observer, [&] {
        return authenticate_auth_code_account_impl(
            config, runtime_factory, authorization, stop_token
        );
    });
}

auth::AuthResult<AuthenticationResult> authenticate_auth_code_account(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const AuthCodeAuthorizationCallback& authorization,
    std::stop_token stop_token
) {
    static const NullObserver observer;
    return authenticate_auth_code_account(
        config, runtime_factory, observer, authorization, stop_token
    );
}

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
    return observe_authentication(observer, [&] {
        return authenticate_device_code_account_impl(
            config, runtime_factory, authorization, stop_token
        );
    });
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
