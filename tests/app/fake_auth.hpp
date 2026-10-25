#pragma once

#include "onedrive/http/http_client.hpp"

#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace onedrive::test::app {

class FakeTokenStore final {
public:
    explicit FakeTokenStore(std::filesystem::path path)
        : path_{std::move(path)} {
    }

    [[nodiscard]] std::optional<std::string> load_refresh_token() const {
        std::ifstream input{path_};
        if (!input) {
            return std::nullopt;
        }
        return std::string{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}
        };
    }

    void save_refresh_token(const std::string& token) const {
        std::ofstream output{path_};
        output << token;
    }

    [[nodiscard]] bool remove_refresh_token() const {
        return std::filesystem::remove(path_);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class FakeAuthenticationTransport final {
public:
    using ResponseOverride = std::function<std::optional<
        onedrive::http::HttpResult>(const onedrive::http::HttpRequest&)>;

    explicit FakeAuthenticationTransport(
        ResponseOverride response_override = {}
    )
        : response_override_{std::move(response_override)} {
    }

    [[nodiscard]] onedrive::http::HttpResult
    perform(const onedrive::http::HttpRequest& request) const {
        if (response_override_) {
            if (auto result = response_override_(request)) {
                return std::move(*result);
            }
        }
        if (request.url.ends_with("/oauth2/v2.0/devicecode")) {
            return onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"device_code":"device-code","user_code":"ABCD-EFGH",)"
                    R"("verification_uri":"https://microsoft.com/link",)"
                    R"("message":"Authenticate the test account",)"
                    R"("expires_in":900,"interval":1})",
            };
        }
        if (request.url.ends_with("/oauth2/v2.0/token")) {
            return onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"token_type":"Bearer","expires_in":3600,)"
                        R"("access_token":"access-token",)"
                        R"("refresh_token":"refresh-token"})",
            };
        }
        if (request.url.ends_with("/me?$select=id,displayName")) {
            return onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"id":"user-id","displayName":"Test User"})",
            };
        }
        if (request.url.ends_with("/me/drive?$select=id,name,driveType")) {
            return onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"id":"drive-id","name":"Test Drive","driveType":"personal"})",
            };
        }
        if (request.url.ends_with(
                "/drives/business-drive-id?$select=id,name,driveType"
            )) {
            return onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"id":"business-drive-id","name":"OneDrive - Contoso","driveType":"business"})",
            };
        }
        if (request.url.ends_with(
                "/drives/b%21sp-library?$select=id,name,driveType"
            )) {
            return onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"id":"b!sp-library","name":"Shared Documents","driveType":"documentLibrary"})",
            };
        }
        if (request.url.ends_with("/me/photo/$value")) {
            return onedrive::http::HttpResponse{
                .status_code = 200,
                .headers =
                    {
                        {.name = "Content-Type", .value = "image/jpeg"},
                    },
                .body = "avatar",
            };
        }
        return std::unexpected(
            onedrive::http::HttpError{
                .message = "unexpected authentication request: " + request.url,
            }
        );
    }

    [[nodiscard]] onedrive::http::HttpResult download(
        const onedrive::http::HttpRequest&,
        const std::filesystem::path&,
        const onedrive::http::DownloadProgress&,
        const onedrive::http::DownloadData&,
        const onedrive::http::DownloadCheckpoint&,
        const onedrive::http::DownloadResponseGate&
    ) const {
        return std::unexpected(
            onedrive::http::HttpError{
                .message = "authentication download was not expected",
            }
        );
    }

private:
    ResponseOverride response_override_;
};

} // namespace onedrive::test::app
