#include "support.hpp"
#include "onedrive/app/authentication.hpp"

namespace {

using namespace onedrive::test::app;

int test_authentication_service() {
    CliFixture fixture;
    const auto config = onedrive::config::Config::load(fixture.config_path);
    const onedrive::app::RuntimeFactory factory{
        onedrive::util::borrowed_proxy, fixture.runtime_factory
    };
    std::optional<onedrive::app::DeviceAuthorization> authorization;
    const auto result = onedrive::app::authenticate_account(
        config,
        factory,
        [&authorization](const onedrive::app::DeviceAuthorization& value) {
            authorization = value;
        }
    );
    if (!result || !authorization || authorization->user_code != "ABCD-EFGH" ||
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
    bool callback_called{false};
    const auto cancelled = onedrive::app::authenticate_account(
        config,
        factory,
        [&stop, &callback_called](const onedrive::app::DeviceAuthorization&) {
            callback_called = true;
            stop.request_stop();
        },
        stop.get_token()
    );
    if (cancelled ||
        cancelled.error().code != onedrive::auth::AuthErrorCode::cancelled ||
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
    for (const std::string_view stage : {"token", "identity", "photo"}) {
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
        const auto result = onedrive::app::authenticate_account(
            config,
            factory,
            [](const onedrive::app::DeviceAuthorization&) {},
            stop.get_token()
        );
        const int expected_requests = stage == "token"      ? 2
                                      : stage == "identity" ? 3
                                                            : 5;
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
    if (test_authentication_service() != EXIT_SUCCESS ||
        test_cancelled_and_throwing_callbacks() != EXIT_SUCCESS ||
        test_identity_cancellation_and_failures() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
