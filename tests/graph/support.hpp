#pragma once

#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "support/http.hpp"
#include "support/common.hpp"
#include "onedrive/http/http_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace onedrive::test::graph {

inline const std::filesystem::path& test_directory() {
    static const onedrive::test::TemporaryDirectory temporary;
    return temporary.path();
}

class FakeTransport final {
public:
    explicit FakeTransport(std::deque<onedrive::http::HttpResult> responses)
        : queued{std::move(responses)} {
    }

    onedrive::http::HttpResult
    perform(const onedrive::http::HttpRequest& request) const {
        return queued.perform(request);
    }

    onedrive::http::HttpResult download(
        const onedrive::http::HttpRequest& request,
        const std::filesystem::path& destination,
        const onedrive::http::DownloadProgress& progress,
        const onedrive::http::DownloadData& data,
        const onedrive::http::DownloadCheckpoint& checkpoint,
        const onedrive::http::DownloadResponseGate& response_gate
    ) const {
        download_requests.push_back(request);
        if (response_gate) {
            ++download_response_gate_count;
        }
        std::string body = download_body;
        long status_code = 200;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> range;
        for (const auto& header : request.headers) {
            if (!header.starts_with("Range: bytes=")) {
                continue;
            }
            const auto separator = header.find('-', 13);
            const auto start = std::stoull(header.substr(13, separator - 13));
            const auto end = std::stoull(header.substr(separator + 1));
            body = download_body.substr(
                static_cast<std::size_t>(start),
                static_cast<std::size_t>(end - start + 1)
            );
            status_code = 206;
            range = {start, end};
        }
        onedrive::http::HttpResponse response{
            .status_code = status_code,
            .received_size = static_cast<std::uint64_t>(body.size()),
        };
        if (range.has_value()) {
            response.headers.push_back({
                .name = "Content-Range",
                .value = std::format(
                    "bytes {}-{}/{}",
                    range->first,
                    range->second,
                    download_body.size()
                ),
            });
        }
        std::optional<onedrive::http::HttpError> transfer_error;
        if (!download_responses.empty()) {
            auto configured = std::move(download_responses.front());
            download_responses.pop_front();
            if (!configured) {
                if (partial_failure_bytes == 0) {
                    return configured;
                }
                const auto retained = std::min(
                    partial_failure_bytes,
                    static_cast<std::uint64_t>(body.size())
                );
                body.resize(static_cast<std::size_t>(retained));
                response.received_size = retained;
                transfer_error = configured.error();
                partial_failure_bytes = 0;
            } else {
                response = std::move(*configured);
            }
        }
        const bool accepted =
            !response_gate ||
            response_gate(response.status_code, response.headers);
        if (accepted && request.download_offset == 0) {
            std::ofstream output{destination, std::ios::binary};
            output << body;
        } else if (accepted) {
            std::fstream output{
                destination, std::ios::binary | std::ios::in | std::ios::out
            };
            output.seekp(static_cast<std::streamoff>(request.download_offset));
            output << body;
        }
        if (accepted && data) {
            data(request.download_offset, std::as_bytes(std::span{body}));
        }
        if (accepted && progress) {
            progress(body.size(), body.size());
        }
        if (request.stop_token.stop_requested()) {
            return std::unexpected(
                onedrive::http::HttpError{
                    .code = onedrive::http::HttpErrorCode::cancelled,
                    .message = "fake download was cancelled",
                }
            );
        }
        if (accepted && checkpoint && response.status_code >= 200 &&
            response.status_code < 300) {
            checkpoint(
                request.download_offset +
                static_cast<std::uint64_t>(body.size())
            );
        }
        if (transfer_error.has_value()) {
            return std::unexpected(std::move(*transfer_error));
        }
        return response;
    }

    mutable std::vector<onedrive::http::HttpRequest> requests;
    mutable onedrive::test::QueuedHttpTransport queued;
    mutable std::vector<onedrive::http::HttpRequest> download_requests;
    mutable std::deque<onedrive::http::HttpResult> download_responses;
    mutable std::size_t download_response_gate_count{0};
    mutable std::uint64_t partial_failure_bytes{0};
    std::string download_body{"download"};
};

class FakeTokenStore final {
public:
    explicit FakeTokenStore(std::optional<std::string> refresh_token)
        : refresh_token_{std::move(refresh_token)} {
    }

    [[nodiscard]] std::optional<std::string> load_refresh_token() const {
        return refresh_token_;
    }

    void save_refresh_token(const std::string& refresh_token) const {
        saved_tokens.push_back(refresh_token);
        refresh_token_ = refresh_token;
    }

    [[nodiscard]] bool remove_refresh_token() const {
        const bool present = refresh_token_.has_value();
        refresh_token_.reset();
        return present;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    mutable std::vector<std::string> saved_tokens;

private:
    mutable std::optional<std::string> refresh_token_;
    std::filesystem::path path_{"/fake/refresh_token"};
};

template <typename Implementation>
std::unique_ptr<onedrive::http::HttpTransport>
wrap_transport(std::unique_ptr<Implementation> implementation) {
    return std::make_unique<onedrive::http::HttpTransport>(
        std::move(implementation)
    );
}

template <typename Implementation>
std::unique_ptr<onedrive::auth::TokenStore>
wrap_token_store(std::unique_ptr<Implementation> implementation) {
    return std::make_unique<onedrive::auth::TokenStore>(
        std::move(implementation)
    );
}

using onedrive::test::fail;

inline onedrive::auth::DeviceAuthOptions auth_options() {
    return {
        .application_id = "client id",
        .tenant_id = "test-tenant",
        .auth_endpoint = "https://login.example.test",
        .scope = "Files.ReadWrite offline_access",
    };
}

inline bool has_header(
    const onedrive::http::HttpRequest& request, const std::string& expected
) {
    for (const auto& header : request.headers) {
        if (header == expected ||
            (expected == "Authorization: ******" &&
             header.starts_with("Authorization: Bearer "))) {
            return true;
        }
    }
    return false;
}

} // namespace onedrive::test::graph
