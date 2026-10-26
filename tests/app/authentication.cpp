#include "support/progress.hpp"
#include "support.hpp"
#include "onedrive/app/authentication.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "app/preflight.hpp"
#include "ui/cli/app/browser_auth.hpp"

#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

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

int test_enterprise_authentication_diagnostics() {
    for (const auto* output : {"text", "json"}) {
        for (const std::string stage : {"devicecode", "token"}) {
            CliFixture fixture;
            if (fixture.authenticate().exit_code !=
                onedrive::exit_code::success) {
                return fail(
                    "diagnostic test could not establish existing credentials"
                );
            }
            const auto token_directory =
                onedrive::account::AccountState::active_token_directory(
                    fixture.state_path
                );
            unsigned int requests = 0;
            fixture.runtime_factory.authentication_response_override =
                [&](
                    const onedrive::http::HttpRequest& request
                ) -> std::optional<onedrive::http::HttpResult> {
                ++requests;
                if (request.url.ends_with("/" + stage)) {
                    return onedrive::http::HttpResponse{
                        .status_code = 400,
                        .body =
                            R"({"error":"invalid_grant","error_description":"organization consent needed","error_codes":[90094],"claims":"claims-secret"})",
                    };
                }
                if (stage == "token" && request.url.ends_with("/devicecode")) {
                    return std::nullopt;
                }
                throw std::logic_error{
                    "failed sign-in continued with another request"
                };
            };
            const auto result = run_application(
                fixture.runtime_factory,
                {"onedrive-cpp",
                 "account",
                 "login",
                 "--config",
                 fixture.config_path.string(),
                 "--ui",
                 "console",
                 "--output",
                 output}
            );
            const auto messages = onedrive::test::read_file(fixture.log_path);
            if (result.exit_code == onedrive::exit_code::success ||
                !messages.contains("AADSTS90094") ||
                !messages.contains("Administrator consent is required") ||
                !messages.contains("not a verified tenant ID") ||
                !messages.contains(
                    stage == "token"
                        ? "Microsoft device authorization failed"
                        : "Microsoft device authorization request failed"
                ) ||
                requests != (stage == "token" ? 2U : 1U) ||
                messages.contains("claims-secret") ||
                onedrive::account::AccountState::active_token_directory(
                    fixture.state_path
                ) != token_directory ||
                onedrive::test::read_file(token_directory / "refresh_token") !=
                    "refresh-token") {
                return fail(
                    "CLI lost enterprise advice or modified existing "
                    "credentials"
                );
            }
        }
    }
    return EXIT_SUCCESS;
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
        result->identity.drive_type != "personal" ||
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

int test_business_authentication() {
    CliFixture fixture;
    auto config = onedrive::config::Config::load(fixture.config_path);
    config.drive_id = "business-drive-id";
    config.drive_ids = {"business-drive-id"};
    const onedrive::app::RuntimeFactory factory{
        onedrive::util::borrowed_proxy, fixture.runtime_factory
    };
    const auto result = onedrive::app::authenticate_account(
        config, factory, [](const onedrive::app::DeviceAuthorization&) {}
    );
    if (!result || result->identity.drive_id != "business-drive-id" ||
        result->identity.drive_type != "business" ||
        result->identity.kind() != onedrive::account::DriveKind::business) {
        return fail(
            "OneDrive for Business authentication did not persist business "
            "drive identity"
        );
    }
    return EXIT_SUCCESS;
}

int test_sharepoint_authentication() {
    CliFixture fixture;
    auto config = onedrive::config::Config::load(fixture.config_path);
    config.drive_id = "b!sp-library";
    config.drive_ids = {"b!sp-library"};
    const onedrive::app::RuntimeFactory factory{
        onedrive::util::borrowed_proxy, fixture.runtime_factory
    };
    const auto result = onedrive::app::authenticate_account(
        config, factory, [](const onedrive::app::DeviceAuthorization&) {}
    );
    if (!result || result->identity.drive_id != "b!sp-library" ||
        result->identity.drive_type != "documentLibrary" ||
        result->identity.kind() !=
            onedrive::account::DriveKind::sharepoint_library) {
        return fail(
            "SharePoint authentication did not persist documentLibrary drive "
            "identity"
        );
    }
    return EXIT_SUCCESS;
}

bool send_loopback_callback(
    std::string_view authorize_url,
    bool declined = false,
    std::string_view expected_body = {}
) {
    constexpr std::string_view prefix = "redirect_uri=http%3A%2F%2Flocalhost%3A";
    const auto port_pos = authorize_url.find(prefix);
    const auto state_pos = authorize_url.find("&state=");
    if (port_pos == std::string_view::npos ||
        state_pos == std::string_view::npos) {
        return false;
    }
    const auto port_view =
        authorize_url.substr(port_pos + prefix.size());
    std::uint16_t port = 0;
    const auto [end, ec] = std::from_chars(
        port_view.data(), port_view.data() + port_view.size(), port
    );
    if (ec != std::errc{} || port == 0) {
        return false;
    }
    const auto state_start = state_pos + 7;
    const auto state_end = authorize_url.find('&', state_start);
    const auto state = authorize_url.substr(
        state_start, state_end - state_start
    );

    const int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0) {
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(
            client_fd,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)
        ) != 0) {
        ::close(client_fd);
        return false;
    }
    const auto query_tail =
        declined ? "&error=access_denied" : "&code=cli-auth-code";
    const auto request = "GET /?state=" + std::string{state} + query_tail +
                         " HTTP/1.1\r\nHost: localhost:" +
                         std::to_string(port) + "\r\nConnection: close\r\n\r\n";
    const auto written =
        ::write(client_fd, request.data(), request.size());
    char response[1024]{};
    const auto read_bytes = ::read(client_fd, response, sizeof(response));
    ::close(client_fd);
    const std::string_view response_view{
        response,
        read_bytes > 0 ? static_cast<std::size_t>(read_bytes) : 0U
    };
    return written == static_cast<ssize_t>(request.size()) && read_bytes > 0 &&
           response_view.contains("200 OK") &&
           (expected_body.empty() || response_view.contains(expected_body));
}

int test_cli_gui_detection_and_auth_flow() {
    {
        CliFixture fixture;
        bool exchanged_auth_code = false;
        fixture.runtime_factory.authentication_response_override =
            [&](
                const onedrive::http::HttpRequest& request
            ) -> std::optional<onedrive::http::HttpResult> {
            if (request.url.ends_with("/devicecode")) {
                throw std::logic_error{
                    "GUI auto login unexpectedly requested a device code"
                };
            }
            if (request.url.ends_with("/token")) {
                if (!request.body.contains(
                        "grant_type=authorization_code"
                    ) ||
                    !request.body.contains("code=cli-auth-code")) {
                    throw std::logic_error{
                        "CLI browser login did not exchange cli-auth-code"
                    };
                }
                exchanged_auth_code = true;
                return onedrive::http::HttpResponse{
                    .status_code = 200,
                    .body =
                        R"({"access_token":"access","refresh_token":"browser-refresh","expires_in":3600})",
                };
            }
            return std::nullopt;
        };
        bool browser_launched = false;
        std::future<bool> callback_delivered;
        const onedrive::app::detail::ScopedBrowserAuthOverride override_gui{
            true,
            [&](std::string_view url) {
                browser_launched = true;
                callback_delivered = std::async(
                    std::launch::async,
                    [target = std::string{url}] {
                        return send_loopback_callback(target);
                    }
                );
                return true;
            }
        };
        const auto result = run_application(
            fixture.runtime_factory,
            {"onedrive-cpp",
             "account",
             "login",
             "--config",
             fixture.config_path.string()}
        );
        if (result.exit_code != onedrive::exit_code::success ||
            !browser_launched || !callback_delivered.valid() ||
            !callback_delivered.get() || !exchanged_auth_code ||
            !result.standard_output.contains(
                "Opened your default browser for Microsoft sign-in."
            )) {
            return fail(
                "auto auth-flow with GUI did not complete browser login:\n" +
                result.standard_output + result.standard_error
            );
        }
    }
    {
        CliFixture fixture;
        fixture.runtime_factory.authentication_response_override =
            [](
                const onedrive::http::HttpRequest& request
            ) -> std::optional<onedrive::http::HttpResult> {
            if (request.url.ends_with("/token")) {
                return onedrive::http::HttpResponse{
                    .status_code = 200,
                    .body =
                        R"({"access_token":"access","refresh_token":"browser-zh-refresh","expires_in":3600})",
                };
            }
            return std::nullopt;
        };
        std::future<bool> callback_delivered;
        const onedrive::app::detail::ScopedBrowserAuthOverride override_gui{
            true,
            [&](std::string_view url) {
                callback_delivered = std::async(
                    std::launch::async,
                    [target = std::string{url}] {
                        return send_loopback_callback(
                            target,
                            false,
                            "已收到授权。请返回 OneDrive C++，可以关闭此标签页。"
                        );
                    }
                );
                return true;
            }
        };
        const auto result = run_application(
            fixture.runtime_factory,
            {"onedrive-cpp",
             "account",
             "login",
             "--language",
             "zh_CN",
             "--config",
             fixture.config_path.string()}
        );
        if (result.exit_code != onedrive::exit_code::success ||
            !callback_delivered.valid() || !callback_delivered.get() ||
            !result.standard_output.contains(
                "已打开默认浏览器进行 Microsoft 登录。"
            ) ||
            !result.standard_output.contains("正在等待浏览器登录授权...")) {
            return fail(
                "Chinese browser login did not localize callback page or "
                "console messages:\n" +
                result.standard_output + result.standard_error
            );
        }
    }
    {
        CliFixture fixture;
        bool browser_attempted = false;
        const onedrive::app::detail::ScopedBrowserAuthOverride override_gui{
            true,
            [&](std::string_view) {
                browser_attempted = true;
                return false;
            }
        };
        const auto result = run_application(
            fixture.runtime_factory,
            {"onedrive-cpp",
             "account",
             "login",
             "--config",
             fixture.config_path.string()}
        );
        if (result.exit_code != onedrive::exit_code::success ||
            !browser_attempted ||
            !result.standard_output.contains(
                "Browser authorization is unavailable; falling back to device "
                "code flow."
            ) ||
            !result.standard_output.contains(
                "Authenticate the test account"
            )) {
            return fail(
                "auto auth-flow did not fall back to device code when browser "
                "launch failed:\n" +
                result.standard_output + result.standard_error
            );
        }
    }
    {
        CliFixture fixture;
        const onedrive::app::detail::ScopedBrowserAuthOverride override_gui{
            true,
            [](std::string_view) -> bool {
                throw std::logic_error{
                    "--auth-flow device-code unexpectedly launched browser"
                };
            }
        };
        const auto result = run_application(
            fixture.runtime_factory,
            {"onedrive-cpp",
             "account",
             "login",
             "--auth-flow",
             "device-code",
             "--config",
             fixture.config_path.string()}
        );
        if (result.exit_code != onedrive::exit_code::success ||
            !result.standard_output.contains(
                "Authenticate the test account"
            )) {
            return fail(
                "--auth-flow device-code did not use device code flow"
            );
        }
    }
    {
        CliFixture fixture;
        fixture.runtime_factory.authentication_response_override =
            [](
                const onedrive::http::HttpRequest& request
            ) -> std::optional<onedrive::http::HttpResult> {
            if (request.url.ends_with("/token")) {
                return onedrive::http::HttpResponse{
                    .status_code = 200,
                    .body =
                        R"({"access_token":"access","refresh_token":"explicit-browser","expires_in":3600})",
                };
            }
            return std::nullopt;
        };
        bool browser_launched = false;
        std::future<bool> callback_delivered;
        const onedrive::app::detail::ScopedBrowserAuthOverride override_no_gui{
            false,
            [&](std::string_view url) {
                browser_launched = true;
                callback_delivered = std::async(
                    std::launch::async,
                    [target = std::string{url}] {
                        return send_loopback_callback(target);
                    }
                );
                return true;
            }
        };
        const auto result = run_application(
            fixture.runtime_factory,
            {"onedrive-cpp",
             "account",
             "login",
             "--auth-flow",
             "auth-code",
             "--config",
             fixture.config_path.string()}
        );
        if (result.exit_code != onedrive::exit_code::success ||
            !browser_launched || !callback_delivered.valid() ||
            !callback_delivered.get()) {
            return fail(
                "--auth-flow auth-code did not run browser flow when GUI was "
                "not auto-detected"
            );
        }
    }
    {
        // auto flow without GUI detected goes straight to device-code flow
        // without attempting browser launch or emitting a fallback notice.
        CliFixture fixture;
        const onedrive::app::detail::ScopedBrowserAuthOverride override_no_gui{
            false,
            [](std::string_view) -> bool {
                throw std::logic_error{
                    "auto flow without GUI unexpectedly launched browser"
                };
            }
        };
        const auto result = run_application(
            fixture.runtime_factory,
            {"onedrive-cpp",
             "account",
             "login",
             "--config",
             fixture.config_path.string()}
        );
        if (result.exit_code != onedrive::exit_code::success ||
            result.standard_output.contains(
                "falling back to device code flow"
            ) ||
            !result.standard_output.contains(
                "Authenticate the test account"
            )) {
            return fail(
                "auto flow without GUI did not use device code directly"
            );
        }
    }
    {
        // Explicit --auth-flow auth-code when browser launcher fails still
        // prints the manual URL and waits for the loopback callback instead of
        // falling back to device code.
        CliFixture fixture;
        fixture.runtime_factory.authentication_response_override =
            [](
                const onedrive::http::HttpRequest& request
            ) -> std::optional<onedrive::http::HttpResult> {
            if (request.url.ends_with("/devicecode")) {
                throw std::logic_error{
                    "--auth-flow auth-code unexpectedly fell back to device "
                    "code"
                };
            }
            if (request.url.ends_with("/token")) {
                return onedrive::http::HttpResponse{
                    .status_code = 200,
                    .body =
                        R"({"access_token":"access","refresh_token":"manual-browser","expires_in":3600})",
                };
            }
            return std::nullopt;
        };
        std::future<bool> callback_delivered;
        const onedrive::app::detail::ScopedBrowserAuthOverride override_manual{
            false,
            [&](std::string_view url) {
                callback_delivered = std::async(
                    std::launch::async,
                    [target = std::string{url}] {
                        return send_loopback_callback(target);
                    }
                );
                return false;
            }
        };
        const auto result = run_application(
            fixture.runtime_factory,
            {"onedrive-cpp",
             "account",
             "login",
             "--auth-flow",
             "auth-code",
             "--output",
             "json",
             "--config",
             fixture.config_path.string()}
        );
        if (result.exit_code != onedrive::exit_code::success ||
            !callback_delivered.valid() || !callback_delivered.get() ||
            !result.standard_output.contains(
                "\"event\":\"browser_authorization\""
            ) ||
            !result.standard_output.contains("in your browser to sign in.")) {
            return fail(
                "--auth-flow auth-code did not support manual URL completion "
                "when browser launch failed:\n" +
                result.standard_output + result.standard_error
            );
        }
    }
    {
        // In auto mode, once the browser launches, an OAuth denial must fail
        // explicitly rather than silently falling back to device code.
        CliFixture fixture;
        fixture.runtime_factory.authentication_response_override =
            [](
                const onedrive::http::HttpRequest& request
            ) -> std::optional<onedrive::http::HttpResult> {
            if (request.url.ends_with("/devicecode")) {
                throw std::logic_error{
                    "declined browser login unexpectedly fell back to device "
                    "code"
                };
            }
            return std::nullopt;
        };
        std::future<bool> callback_delivered;
        const onedrive::app::detail::ScopedBrowserAuthOverride override_gui{
            true,
            [&](std::string_view url) {
                callback_delivered = std::async(
                    std::launch::async,
                    [target = std::string{url}] {
                        return send_loopback_callback(target, true);
                    }
                );
                return true;
            }
        };
        const auto result = run_application(
            fixture.runtime_factory,
            {"onedrive-cpp",
             "account",
             "login",
             "--config",
             fixture.config_path.string()}
        );
        if (result.exit_code != onedrive::exit_code::failure ||
            !callback_delivered.valid() || !callback_delivered.get() ||
            result.standard_output.contains(
                "falling back to device code flow"
            ) ||
            onedrive::account::AccountState::find_active_token_directory(
                fixture.state_path
            )) {
            return fail(
                "declined browser login in auto mode did not fail cleanly:\n" +
                result.standard_output + result.standard_error
            );
        }
    }
    {
        // Real PATH lookup + posix_spawn execution of xdg-open (without
        // ScopedBrowserAuthOverride): verifies both exit-0 browser launch and
        // exit-1 spawn failure detection.
        CliFixture fixture;
        fixture.runtime_factory.authentication_response_override =
            [](
                const onedrive::http::HttpRequest& request
            ) -> std::optional<onedrive::http::HttpResult> {
            if (request.url.ends_with("/token")) {
                return onedrive::http::HttpResponse{
                    .status_code = 200,
                    .body =
                        R"({"access_token":"access","refresh_token":"spawned-browser","expires_in":3600})",
                };
            }
            return std::nullopt;
        };
        const auto bin_dir = fixture.state_path.parent_path() / "bin";
        std::filesystem::create_directories(bin_dir);
        const auto captured_url_file =
            fixture.state_path.parent_path() / "captured_url.txt";
        const auto xdg_open_script = bin_dir / "xdg-open";
        onedrive::test::write_file(
            xdg_open_script,
            "#!/bin/sh\nprintf '%s' \"$1\" > \"" +
                captured_url_file.string() + ".tmp\" && mv \"" +
                captured_url_file.string() + ".tmp\" \"" +
                captured_url_file.string() + "\"\nexit 0\n"
        );
        if (::chmod(xdg_open_script.c_str(), 0700) != 0) {
            return fail("could not mark fake xdg-open executable");
        }
        const char* old_path_raw = std::getenv("PATH");
        const std::string old_path = old_path_raw ? old_path_raw : "";
        struct PathGuard {
            std::string previous;
            ~PathGuard() {
                // NOLINTNEXTLINE(concurrency-mt-unsafe)
                ::setenv("PATH", previous.c_str(), 1);
            }
        } const path_guard{old_path};
        const std::string new_path =
            bin_dir.string() + (old_path.empty() ? "" : ":" + old_path);
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        ::setenv("PATH", new_path.c_str(), 1);

        auto deliver_from_file = [&] {
            return std::async(std::launch::async, [&] {
                for (int i = 0; i < 200; ++i) {
                    if (std::filesystem::exists(captured_url_file)) {
                        const auto url =
                            onedrive::test::read_file(captured_url_file);
                        std::filesystem::remove(captured_url_file);
                        return send_loopback_callback(url);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{10});
                }
                return false;
            });
        };
        auto delivered = deliver_from_file();
        const auto ok_result = run_application(
            fixture.runtime_factory,
            {"onedrive-cpp",
             "account",
             "login",
             "--auth-flow",
             "auth-code",
             "--config",
             fixture.config_path.string()}
        );
        if (ok_result.exit_code != onedrive::exit_code::success ||
            !delivered.get() ||
            !ok_result.standard_output.contains(
                "Opened your default browser for Microsoft sign-in."
            )) {
            return fail(
                "real posix_spawn xdg-open did not launch browser:\n" +
                ok_result.standard_output + ok_result.standard_error
            );
        }

        onedrive::test::write_file(
            xdg_open_script,
            "#!/bin/sh\nprintf '%s' \"$1\" > \"" +
                captured_url_file.string() + ".tmp\" && mv \"" +
                captured_url_file.string() + ".tmp\" \"" +
                captured_url_file.string() + "\"\nexit 1\n"
        );
        if (::chmod(xdg_open_script.c_str(), 0700) != 0) {
            return fail("could not mark failing fake xdg-open executable");
        }
        auto failed_spawn_delivered = deliver_from_file();
        const auto fail_spawn_result = run_application(
            fixture.runtime_factory,
            {"onedrive-cpp",
             "account",
             "login",
             "--auth-flow",
             "auth-code",
             "--config",
             fixture.config_path.string()}
        );
        if (fail_spawn_result.exit_code != onedrive::exit_code::success ||
            !failed_spawn_delivered.get() ||
            !fail_spawn_result.standard_output.contains(
                "in your browser to sign in."
            )) {
            return fail(
                "real posix_spawn xdg-open exit-1 was not detected:\n" +
                fail_spawn_result.standard_output +
                fail_spawn_result.standard_error
            );
        }
    }
    return EXIT_SUCCESS;
}
} // namespace

int main() {
    onedrive::test::ScenarioProgress progress{9};
    if (progress.run(
            "shared_credentials_and_runtime_lock",
            test_shared_credentials_and_runtime_lock
        ) != EXIT_SUCCESS ||
        progress.run(
            "auth_code_authentication", test_auth_code_authentication
        ) != EXIT_SUCCESS ||
        progress.run(
            "cli_gui_detection_and_auth_flow",
            test_cli_gui_detection_and_auth_flow
        ) != EXIT_SUCCESS ||
        progress.run(
            "business_authentication", test_business_authentication
        ) != EXIT_SUCCESS ||
        progress.run(
            "sharepoint_authentication", test_sharepoint_authentication
        ) != EXIT_SUCCESS ||
        progress.run("authentication_service", test_authentication_service) !=
            EXIT_SUCCESS ||
        progress.run(
            "cancelled_and_throwing_callbacks",
            test_cancelled_and_throwing_callbacks
        ) != EXIT_SUCCESS ||
        progress.run(
            "identity_cancellation_and_failures",
            test_identity_cancellation_and_failures
        ) != EXIT_SUCCESS ||
        progress.run(
            "enterprise_authentication_diagnostics",
            test_enterprise_authentication_diagnostics
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return progress.finish();
}
