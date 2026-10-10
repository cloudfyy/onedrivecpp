#include "support.hpp"
#include "onedrive/app/authentication.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "app/preflight.hpp"

#include <vector>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace onedrive::test::app;

class RecordingObserver final : public onedrive::events::Observer {
public:
    [[nodiscard]] std::vector<onedrive::events::OperationState>
    operation_states() const {
        std::vector<onedrive::events::OperationState> states;
        for (const auto& event : events) {
            if (const auto* state =
                    std::get_if<onedrive::events::OperationStateEvent>(
                        &event
                    )) {
                states.push_back(state->state);
            }
        }
        return states;
    }

private:
    void on_event(const onedrive::events::Event& event) const override {
        events.push_back(event);
    }

    mutable std::vector<onedrive::events::Event> events;
};

onedrive::auth::AuthResult<void>
authorize_browser(onedrive::auth::AuthCodeSession& session) {
    const auto url = session.begin("http://localhost:54321/");
    if (!url)
        return std::unexpected(url.error());
    const auto start = url->find("&state=") + 7;
    const auto state = url->substr(start, url->find('&', start) - start);
    return session.accept_callback(
        "http://localhost:54321/?state=" + state + "&code=code"
    );
}

int test_auth_code_authentication() {
    CliFixture fixture;
    const auto config = onedrive::config::Config::load(fixture.config_path);
    const onedrive::app::RuntimeFactory factory{
        onedrive::util::borrowed_proxy, fixture.runtime_factory
    };
    bool exchanged = false;
    fixture.runtime_factory.authentication_response_override =
        [&](
            const onedrive::http::HttpRequest& request
        ) -> std::optional<onedrive::http::HttpResult> {
        if (request.url.ends_with("/devicecode")) {
            throw std::logic_error{"browser authentication used device flow"};
        }
        if (request.url.ends_with("/token")) {
            exchanged =
                request.body.contains("grant_type=authorization_code") &&
                request.body.contains("code_verifier=");
        }
        return std::nullopt;
    };
    RecordingObserver observer;
    const auto result = onedrive::app::authenticate_auth_code_account(
        config,
        factory,
        observer,
        [](onedrive::auth::AuthCodeSession& session, std::stop_token) {
            return authorize_browser(session);
        }
    );
    if (!result || !exchanged || result->identity.drive_id != "drive-id" ||
        onedrive::test::read_file(result->token_directory / "refresh_token") !=
            "refresh-token" ||
        observer.operation_states() !=
            std::vector{
                onedrive::events::OperationState::authenticating,
                onedrive::events::OperationState::ready,
            }) {
        return fail(
            "browser authentication did not activate the account or report "
            "state"
        );
    }
    for (const auto error :
         {onedrive::auth::AuthErrorCode::cancelled,
          onedrive::auth::AuthErrorCode::authorization_declined}) {
        RecordingObserver failure_observer;
        exchanged = false;
        const auto failed = onedrive::app::authenticate_auth_code_account(
            config,
            factory,
            failure_observer,
            [error](
                onedrive::auth::AuthCodeSession&, std::stop_token
            ) -> onedrive::auth::AuthResult<void> {
                return std::unexpected(
                    onedrive::auth::AuthError{
                        .code = error, .message = "callback failed"
                    }
                );
            }
        );
        if (failed || exchanged || failed.error().code != error ||
            failure_observer.operation_states().back() !=
                (error == onedrive::auth::AuthErrorCode::cancelled
                     ? onedrive::events::OperationState::idle
                     : onedrive::events::OperationState::failed) ||
            onedrive::test::read_file(
                result->token_directory / "refresh_token"
            ) != "refresh-token") {
            return fail(
                "browser callback failure exchanged a code or changed saved "
                "credentials"
            );
        }
    }
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = onedrive::app::authenticate_auth_code_account(
        config,
        factory,
        [](onedrive::auth::AuthCodeSession&,
           std::stop_token) -> onedrive::auth::AuthResult<void> {
            throw std::logic_error{"pre-cancelled browser callback invoked"};
        },
        stop.get_token()
    );
    if (cancelled ||
        cancelled.error().code != onedrive::auth::AuthErrorCode::cancelled ||
        !onedrive::test::throws_with<std::invalid_argument>(
            [&] {
                static_cast<void>(onedrive::app::authenticate_auth_code_account(
                    config, factory, {}
                ));
            },
            "authorization callback"
        )) {
        return fail(
            "browser authentication did not reject cancellation or a missing "
            "callback"
        );
    }
    for (const bool callback_cancelled : {true, false}) {
        std::stop_source cancellation;
        fixture.runtime_factory.authentication_response_override =
            [](
                const onedrive::http::HttpRequest&
            ) -> std::optional<onedrive::http::HttpResult> {
            return std::unexpected(
                onedrive::http::HttpError{
                    .message = "exchange transport failed"
                }
            );
        };
        RecordingObserver failure_observer;
        const auto failed = onedrive::app::authenticate_auth_code_account(
            config,
            factory,
            failure_observer,
            [&](onedrive::auth::AuthCodeSession& session,
                std::stop_token token) -> onedrive::auth::AuthResult<void> {
                if (token != cancellation.get_token()) {
                    throw std::logic_error{
                        "browser callback lost the stop token"
                    };
                }
                const auto result = authorize_browser(session);
                if (callback_cancelled) {
                    cancellation.request_stop();
                }
                return result;
            },
            cancellation.get_token()
        );
        if (failed ||
            failed.error().code !=
                (callback_cancelled
                     ? onedrive::auth::AuthErrorCode::cancelled
                     : onedrive::auth::AuthErrorCode::transport) ||
            failure_observer.operation_states().back() !=
                (callback_cancelled
                     ? onedrive::events::OperationState::idle
                     : onedrive::events::OperationState::failed) ||
            onedrive::test::read_file(
                result->token_directory / "refresh_token"
            ) != "refresh-token") {
            return fail(
                "cancelled or failed browser code exchange changed saved "
                "credentials"
            );
        }
    }
    RecordingObserver exception_observer;
    if (!onedrive::test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(onedrive::app::authenticate_auth_code_account(
                    config,
                    factory,
                    exception_observer,
                    [](onedrive::auth::AuthCodeSession&,
                       std::stop_token) -> onedrive::auth::AuthResult<void> {
                        throw std::runtime_error{"browser callback failed"};
                    }
                ));
            },
            "browser callback failed"
        ) ||
        exception_observer.operation_states().back() !=
            onedrive::events::OperationState::failed) {
        return fail(
            "browser callback exception was swallowed or did not report failure"
        );
    }
    return EXIT_SUCCESS;
}

int test_shared_credentials_and_runtime_lock() {
    CliFixture fixture;
    const auto config = onedrive::config::Config::load(fixture.config_path);
    const onedrive::app::RuntimeFactory factory{
        onedrive::util::borrowed_proxy, fixture.runtime_factory
    };
    const auto device = onedrive::app::authenticate_account(
        config, factory, [](const onedrive::app::DeviceAuthorization&) {}
    );
    if (!device)
        return fail("device login could not initialize shared credentials");
    {
        const onedrive::app::detail::RuntimePreflight lock{
            config, onedrive::app::detail::Operation::logout
        };
        const auto child = ::fork();
        if (child == -1) {
            return fail("could not fork competing authentication process");
        }
        if (child == 0) {
            const bool rejected =
                onedrive::test::throws_with<std::runtime_error>(
                    [&] {
                        static_cast<
                            void>(onedrive::app::authenticate_auth_code_account(
                            config,
                            factory,
                            [](onedrive::auth::AuthCodeSession&,
                               std::stop_token)
                                -> onedrive::auth::AuthResult<void> {
                                throw std::logic_error{
                                    "competing process started authorization"
                                };
                            }
                        ));
                    },
                    "another onedrive-cpp process"
                );
            ::_exit(rejected ? EXIT_SUCCESS : EXIT_FAILURE);
        }
        int status = 0;
        pid_t waited;
        do {
            waited = ::waitpid(child, &status, 0);
        } while (waited == -1 && errno == EINTR);
        if (waited != child || !WIFEXITED(status) ||
            WEXITSTATUS(status) != EXIT_SUCCESS) {
            return fail(
                "competing process was not rejected by the shared "
                "authentication lock"
            );
        }
        if (!onedrive::test::throws_with<std::runtime_error>(
                [&] {
                    static_cast<void>(
                        onedrive::app::authenticate_auth_code_account(
                            config,
                            factory,
                            [](onedrive::auth::AuthCodeSession&,
                               std::stop_token)
                                -> onedrive::auth::AuthResult<void> {
                                throw std::logic_error{
                                    "locked login invoked authorization"
                                };
                            }
                        )
                    );
                },
                "another onedrive-cpp process"
            ) ||
            !onedrive::test::throws_with<std::runtime_error>(
                [&] {
                    static_cast<void>(onedrive::app::authenticate_account(
                        config,
                        factory,
                        [](const onedrive::app::DeviceAuthorization&) {
                            throw std::logic_error{
                                "locked device login invoked "
                                "authorization"
                            };
                        }
                    ));
                },
                "another onedrive-cpp process"
            ) ||
            onedrive::test::read_file(
                device->token_directory / "refresh_token"
            ) != "refresh-token") {
            return fail(
                "locked login changed shared credentials or started "
                "authentication"
            );
        }
    }
    fixture.runtime_factory.authentication_response_override =
        [](
            const onedrive::http::HttpRequest& request
        ) -> std::optional<onedrive::http::HttpResult> {
        if (request.url.ends_with("/token")) {
            return onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"access_token":"access","refresh_token":"auth-code-refresh","expires_in":3600})",
            };
        }
        return std::nullopt;
    };
    const auto code = onedrive::app::authenticate_auth_code_account(
        config,
        factory,
        [&](onedrive::auth::AuthCodeSession& session, std::stop_token) {
            // The login holds the same lock used by CLI logout and
            // refresh.
            if (!onedrive::test::throws_with<std::runtime_error>(
                    [&] {
                        const onedrive::app::detail::RuntimePreflight other{
                            config, onedrive::app::detail::Operation::logout
                        };
                    },
                    "another onedrive-cpp process"
                )) {
                throw std::logic_error{
                    "auth code login did not hold runtime lock"
                };
            }
            return authorize_browser(session);
        }
    );
    if (!code || code->token_directory != device->token_directory ||
        onedrive::test::read_file(
            onedrive::account::AccountState::active_token_directory(
                config.state_directory
            ) /
            "refresh_token"
        ) != "auth-code-refresh") {
        return fail(
            "auth code and device login did not share account "
            "credentials"
        );
    }
    const onedrive::app::detail::RuntimePreflight released{
        config, onedrive::app::detail::Operation::logout
    };
    return EXIT_SUCCESS;
}

int test_authentication_service() {
    CliFixture fixture;
    const auto config = onedrive::config::Config::load(fixture.config_path);
    const onedrive::app::RuntimeFactory factory{
        onedrive::util::borrowed_proxy, fixture.runtime_factory
    };
    RecordingObserver observer;
    std::optional<onedrive::app::DeviceAuthorization> authorization;
    const auto result = onedrive::app::authenticate_account(
        config,
        factory,
        observer,
        [&authorization](const onedrive::app::DeviceAuthorization& value) {
            authorization = value;
        }
    );
    if (!result ||
        observer.operation_states() !=
            std::vector<onedrive::events::OperationState>{
                onedrive::events::OperationState::authenticating,
                onedrive::events::OperationState::ready,
            } ||
        !authorization || authorization->user_code != "ABCD-EFGH" ||
        authorization->verification_uri != "https://microsoft.com/link" ||
        authorization->expires_in != std::chrono::seconds{900} ||
        authorization->message != "Authenticate the test account" ||
        result->identity.user_id != "user-id" ||
        result->identity.drive_id != "drive-id" ||
        onedrive::test::read_file(result->token_directory / "refresh_token") !=
            "refresh-token") {
        return fail("authentication service did not return owning login data");
    }

    const auto original_token =
        onedrive::test::read_file(result->token_directory / "refresh_token");
    std::stop_source stop;
    RecordingObserver cancellation_observer;
    bool callback_called{false};
    const auto cancelled = onedrive::app::authenticate_account(
        config,
        factory,
        cancellation_observer,
        [&stop, &callback_called](const onedrive::app::DeviceAuthorization&) {
            callback_called = true;
            stop.request_stop();
        },
        stop.get_token()
    );
    if (cancelled ||
        cancelled.error().code != onedrive::auth::AuthErrorCode::cancelled ||
        cancellation_observer.operation_states() !=
            std::vector<onedrive::events::OperationState>{
                onedrive::events::OperationState::authenticating,
                onedrive::events::OperationState::idle,
            } ||
        !callback_called ||
        onedrive::test::read_file(result->token_directory / "refresh_token") !=
            original_token) {
        return fail("cancelled authentication changed existing credentials");
    }
    return EXIT_SUCCESS;
}

int test_cancelled_and_throwing_callbacks() {
    CliFixture fixture;
    const auto config = onedrive::config::Config::load(fixture.config_path);
    const onedrive::app::RuntimeFactory factory{
        onedrive::util::borrowed_proxy, fixture.runtime_factory
    };
    std::stop_source stop;
    stop.request_stop();
    const auto result = onedrive::app::authenticate_account(
        config,
        factory,
        [](const onedrive::app::DeviceAuthorization&) {
            throw std::logic_error{"cancelled request invoked callback"};
        },
        stop.get_token()
    );
    if (result ||
        result.error().code != onedrive::auth::AuthErrorCode::cancelled ||
        onedrive::account::AccountState::find_active_token_directory(
            fixture.state_path
        )) {
        return fail("pre-cancelled authentication activated an account");
    }

    if (!onedrive::test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(onedrive::app::authenticate_account(
                    config,
                    factory,
                    [](const onedrive::app::DeviceAuthorization&) {
                        throw std::runtime_error{"callback failed"};
                    }
                ));
            },
            "callback failed"
        ) ||
        onedrive::account::AccountState::find_active_token_directory(
            fixture.state_path
        )) {
        return fail("callback failure was swallowed or activated an account");
    }
    return EXIT_SUCCESS;
}

int test_identity_cancellation_and_failures() {
    for (const auto& [browser, stage] :
         std::vector<std::pair<bool, std::string_view>>{
             {false, "token"},
             {false, "identity"},
             {false, "photo"},
             {true, "token"},
             {true, "identity"},
             {true, "photo"},
         }) {
        CliFixture fixture;
        const auto config = onedrive::config::Config::load(fixture.config_path);
        std::stop_source stop;
        int requests{0};
        fixture.runtime_factory.authentication_response_override =
            [&](
                const onedrive::http::HttpRequest& request
            ) -> std::optional<onedrive::http::HttpResult> {
            ++requests;
            if (request.stop_token != stop.get_token()) {
                throw std::logic_error{
                    "authentication request lost stop token"
                };
            }
            if (!onedrive::test::throws_with<std::runtime_error>(
                    [&] {
                        const onedrive::app::detail::RuntimePreflight other{
                            config, onedrive::app::detail::Operation::logout
                        };
                    },
                    "another onedrive-cpp process"
                )) {
                throw std::logic_error{
                    "authentication request did not retain the runtime lock"
                };
            }
            if ((stage == "token" && request.url.ends_with("/token")) ||
                (stage == "identity" &&
                 request.url.ends_with("/me?$select=id,displayName")) ||
                (stage == "photo" &&
                 request.url.ends_with("/me/photo/$value"))) {
                stop.request_stop();
                return std::unexpected(
                    onedrive::http::HttpError{
                        .code = onedrive::http::HttpErrorCode::cancelled,
                        .message = "test request cancelled",
                    }
                );
            }
            return std::nullopt;
        };
        const onedrive::app::RuntimeFactory factory{
            onedrive::util::borrowed_proxy, fixture.runtime_factory
        };
        const auto result =
            browser ? onedrive::app::authenticate_auth_code_account(
                          config,
                          factory,
                          [](onedrive::auth::AuthCodeSession& session,
                             std::stop_token) {
                              return authorize_browser(session);
                          },
                          stop.get_token()
                      )
                    : onedrive::app::authenticate_account(
                          config,
                          factory,
                          [](const onedrive::app::DeviceAuthorization&) {},
                          stop.get_token()
                      );
        const int expected_requests = (stage == "token"      ? 2
                                       : stage == "identity" ? 3
                                                             : 5) -
                                      (browser ? 1 : 0);
        if (result ||
            result.error().code != onedrive::auth::AuthErrorCode::cancelled ||
            requests != expected_requests ||
            onedrive::account::AccountState::find_active_token_directory(
                fixture.state_path
            )) {
            return fail(
                "HTTP cancellation activated an account or continued "
                "requests"
            );
        }
        const onedrive::app::detail::RuntimePreflight released{
            config, onedrive::app::detail::Operation::logout
        };
    }

    CliFixture fixture;
    const auto config = onedrive::config::Config::load(fixture.config_path);
    fixture.runtime_factory.authentication_response_override =
        [](
            const onedrive::http::HttpRequest& request
        ) -> std::optional<onedrive::http::HttpResult> {
        if (request.url.ends_with("/devicecode")) {
            return std::unexpected(
                onedrive::http::HttpError{
                    .message = "authentication transport failed",
                }
            );
        }
        return std::nullopt;
    };
    const onedrive::app::RuntimeFactory factory{
        onedrive::util::borrowed_proxy, fixture.runtime_factory
    };
    const auto failed = onedrive::app::authenticate_account(
        config, factory, [](const onedrive::app::DeviceAuthorization&) {
            throw std::logic_error{"failed request invoked callback"};
        }
    );
    if (failed ||
        failed.error().code != onedrive::auth::AuthErrorCode::transport ||
        failed.error().message != "authentication transport failed" ||
        onedrive::account::AccountState::find_active_token_directory(
            fixture.state_path
        )) {
        return fail("authentication transport failure was hidden or persisted");
    }
    for (const auto& [error, expected] :
         std::vector<std::pair<std::string, onedrive::auth::AuthErrorCode>>{
             {"authorization_declined",
              onedrive::auth::AuthErrorCode::authorization_declined},
             {"expired_token", onedrive::auth::AuthErrorCode::expired},
         }) {
        fixture.runtime_factory.authentication_response_override =
            [&](
                const onedrive::http::HttpRequest& request
            ) -> std::optional<onedrive::http::HttpResult> {
            if (request.url.ends_with("/token")) {
                return onedrive::http::HttpResponse{
                    .status_code = 400,
                    .body = "{\"error\":\"" + error + "\"}",
                };
            }
            return std::nullopt;
        };
        const auto denied = onedrive::app::authenticate_account(
            config, factory, [](const onedrive::app::DeviceAuthorization&) {}
        );
        if (denied || denied.error().code != expected ||
            onedrive::account::AccountState::find_active_token_directory(
                fixture.state_path
            )) {
            return fail("declined or expired authentication was persisted");
        }
    }
    fixture.runtime_factory.authentication_response_override =
        [](
            const onedrive::http::HttpRequest& request
        ) -> std::optional<onedrive::http::HttpResult> {
        if (request.url.ends_with("/me?$select=id,displayName")) {
            return std::unexpected(
                onedrive::http::HttpError{
                    .message = "identity transport failed",
                }
            );
        }
        return std::nullopt;
    };
    if (!onedrive::test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(onedrive::app::authenticate_account(
                    config,
                    factory,
                    [](const onedrive::app::DeviceAuthorization&) {}
                ));
            },
            "identity transport failed"
        ) ||
        onedrive::account::AccountState::find_active_token_directory(
            fixture.state_path
        )) {
        return fail("identity failure was swallowed or activated an account");
    }
    return EXIT_SUCCESS;
}
} // namespace

int main() {
    if (test_shared_credentials_and_runtime_lock() != EXIT_SUCCESS ||
        test_auth_code_authentication() != EXIT_SUCCESS ||
        test_authentication_service() != EXIT_SUCCESS ||
        test_cancelled_and_throwing_callbacks() != EXIT_SUCCESS ||
        test_identity_cancellation_and_failures() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
