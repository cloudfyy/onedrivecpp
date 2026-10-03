#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/http/http_client.hpp"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class FakeTransport final : public onedrive::http::HttpTransport {
public:
    explicit FakeTransport(std::deque<onedrive::http::HttpResult> responses)
        : responses_{std::move(responses)} {}

    onedrive::http::HttpResult perform(
        const onedrive::http::HttpRequest& request
    ) const override {
        requests.push_back(request);
        if (responses_.empty()) {
            return std::unexpected(
                onedrive::http::HttpError{.message = "no fake response available"}
            );
        }
        auto response = std::move(responses_.front());
        responses_.pop_front();
        return response;
    }

    onedrive::http::HttpResult download(
        const onedrive::http::HttpRequest&,
        const std::filesystem::path&,
        const onedrive::http::DownloadProgress&
    ) const override {
        return std::unexpected(
            onedrive::http::HttpError{.message = "download was not expected"}
        );
    }

    mutable std::vector<onedrive::http::HttpRequest> requests;

private:
    mutable std::deque<onedrive::http::HttpResult> responses_;
};

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_{
              std::filesystem::temp_directory_path() /
              ("onedrive-cpp-auth-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
          } {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

onedrive::auth::DeviceAuthOptions test_options() {
    return {
        .application_id = "client id",
        .tenant_id = "test-tenant",
        .auth_endpoint = "https://login.example.test",
        .scope = "Files.ReadWrite offline_access",
    };
}

int test_device_flow() {
    FakeTransport transport{{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"({"device_code":"device-secret","user_code":"ABCD-EFGH",)"
                R"("verification_uri":"https://microsoft.com/devicelogin",)"
                R"("expires_in":900,"interval":1})",
        },
        onedrive::http::HttpResponse{
            .status_code = 400,
            .body =
                R"({"error":"authorization_pending",)"
                R"("error_description":"Waiting for the user"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"({"token_type":"Bearer","expires_in":3600,)"
                R"("access_token":"access-secret","refresh_token":"refresh-secret"})",
        },
    }};

    std::vector<std::chrono::seconds> sleeps;
    const auto fixed_time = std::chrono::steady_clock::now();
    onedrive::auth::DeviceAuthClient client{
        transport,
        test_options(),
        [&sleeps](std::chrono::seconds duration) { sleeps.push_back(duration); },
        [fixed_time] { return fixed_time; },
    };

    auto code = client.request_device_code();
    if (!code || code->user_code != "ABCD-EFGH") {
        return fail("device code response was not parsed");
    }
    auto tokens = client.poll_for_token(*code);
    if (!tokens || tokens->access_token != "access-secret" ||
        tokens->refresh_token != "refresh-secret") {
        return fail("device token response was not parsed");
    }
    if (sleeps !=
        std::vector{std::chrono::seconds{1}, std::chrono::seconds{1}}) {
        return fail("device flow did not wait before each token request");
    }
    if (transport.requests.size() != 3 ||
        transport.requests[0].url !=
            "https://login.example.test/test-tenant/oauth2/v2.0/devicecode" ||
        !transport.requests[0].body.contains("client_id=client%20id") ||
        !transport.requests[2].body.contains(
            "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code"
        )) {
        return fail("device flow request was not encoded correctly");
    }
    return EXIT_SUCCESS;
}

int test_refresh_and_token_store() {
    FakeTransport transport{{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"expires_in":3600,"access_token":"new-access"})",
        },
    }};
    onedrive::auth::DeviceAuthClient client{transport, test_options()};
    auto tokens = client.refresh_access_token("existing-refresh");
    if (!tokens || tokens->refresh_token != "existing-refresh") {
        return fail("refresh response did not preserve the existing refresh token");
    }

    TemporaryDirectory temporary_directory;
    onedrive::auth::FileTokenStore store{temporary_directory.path()};
    store.save_refresh_token(tokens->refresh_token);
    store.save_refresh_token("rotated-refresh");
    const auto loaded = store.load_refresh_token();
    if (!loaded || *loaded != "rotated-refresh") {
        return fail("refresh token was not persisted");
    }

    const auto permissions = std::filesystem::status(store.path()).permissions();
    const auto expected =
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    if ((permissions & std::filesystem::perms::all) != expected) {
        return fail("refresh token permissions are not 0600");
    }
    std::filesystem::permissions(
        store.path(),
        expected | std::filesystem::perms::group_read
    );
    try {
        static_cast<void>(store.load_refresh_token());
        return fail("overly broad refresh token permissions were accepted");
    } catch (const std::runtime_error&) {
    }
    std::filesystem::permissions(store.path(), expected);
    if (!store.remove_refresh_token() || store.load_refresh_token()) {
        return fail("refresh token was not removed");
    }
    std::filesystem::create_symlink("/etc/passwd", store.path());
    try {
        static_cast<void>(store.load_refresh_token());
        return fail("symbolic-link refresh token was accepted");
    } catch (const std::runtime_error&) {
    }
    return EXIT_SUCCESS;
}

int test_declined_authorization() {
    FakeTransport transport{{
        onedrive::http::HttpResponse{
            .status_code = 400,
            .body =
                R"({"error":"authorization_declined",)"
                R"("error_description":"The user declined"})",
        },
    }};
    const auto fixed_time = std::chrono::steady_clock::now();
    onedrive::auth::DeviceAuthClient client{
        transport,
        test_options(),
        [](std::chrono::seconds) {},
        [fixed_time] { return fixed_time; },
    };
    const onedrive::auth::DeviceCode code{
        .device_code = "device-secret",
        .user_code = "ABCD-EFGH",
        .verification_uri = "https://microsoft.com/devicelogin",
        .message = {},
        .expires_in = std::chrono::seconds{900},
        .polling_interval = std::chrono::seconds{1},
    };

    const auto result = client.poll_for_token(code);
    if (result ||
        result.error().code != onedrive::auth::AuthErrorCode::authorization_declined) {
        return fail("declined authorization was not reported");
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (const int result = test_device_flow(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_refresh_and_token_store(); result != EXIT_SUCCESS) {
        return result;
    }
    return test_declined_authorization();
}
