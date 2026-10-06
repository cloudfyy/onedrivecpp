#pragma once

#include "onedrive/graph/graph_client.hpp"
#include "onedrive/http/http_client.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace onedrive::graph::client_detail {

using Json = nlohmann::json;

[[nodiscard]] std::string percent_encode_remote_path(std::string_view path);
[[nodiscard]] std::string graph_error_message(const Json& response, long status_code);
[[nodiscard]] std::string graph_error_code(const Json& response);
[[nodiscard]] bool upload_quota_error(const Json& response, long status_code);
[[noreturn]] void throw_upload_response_error(const Json& response, long status_code);
[[nodiscard]] std::string normalized_endpoint(std::string endpoint);
[[nodiscard]] std::string graph_drive_prefix(const GraphOptions& options);
[[nodiscard]] bool successful_status(long status_code) noexcept;
[[nodiscard]] std::uint64_t effective_upload_rate(
    const http::UploadTransportOptions& options
);
[[nodiscard]] Json parse_graph_json(
    const http::HttpResponse& response,
    std::string_view description = {}
);
[[nodiscard]] std::optional<std::chrono::seconds> retry_after(
    const http::HttpResponse& response
);
[[nodiscard]] std::optional<std::string> header_value(
    const http::HttpResponse& response,
    std::string_view name
);
[[nodiscard]] std::chrono::seconds fallback_retry_delay(
    const GraphOptions& options,
    std::size_t retry
);
[[nodiscard]] bool retryable_status(long status_code);
[[nodiscard]] bool stale_download_url_status(long status_code);
[[nodiscard]] http::HttpResult cancelled_http_result();
[[nodiscard]] bool wait_for_retry(
    std::chrono::seconds delay,
    const MicrosoftGraphClient::SleepFunction& sleep,
    const std::stop_token& stop_token
);

template <typename Fetch>
std::vector<Json> paged_graph_values(
    std::string next_url,
    const std::string& endpoint,
    std::string_view description,
    Fetch&& fetch
) {
    const std::string allowed_url_prefix = endpoint + "/";
    std::unordered_set<std::string> visited_urls;
    std::vector<Json> values;
    while (!next_url.empty()) {
        if (!next_url.starts_with(allowed_url_prefix) ||
            !visited_urls.insert(next_url).second) {
            throw std::runtime_error(
                "Microsoft Graph returned an invalid " +
                std::string{description} + " pagination URL"
            );
        }
        const auto document = fetch(next_url);
        try {
            const auto& page = document.at("value");
            if (!page.is_array()) {
                throw std::runtime_error(
                    "Microsoft Graph " + std::string{description} +
                    " response field 'value' is not an array"
                );
            }
            values.insert(values.end(), page.begin(), page.end());
            next_url.clear();
            if (const auto next = document.find("@odata.nextLink");
                next != document.end()) {
                if (!next->is_string()) {
                    throw std::runtime_error(
                        "Microsoft Graph returned an invalid " +
                        std::string{description} + " pagination URL"
                    );
                }
                next_url = next->template get<std::string>();
            }
        } catch (const Json::exception& error) {
            throw std::runtime_error(
                "Microsoft Graph " + std::string{description} +
                " response is missing required data: " + error.what()
            );
        }
    }
    return values;
}

enum class TransportErrorRetry {
    disabled,
    enabled,
};

template <typename Operation>
http::HttpResult perform_with_retries(
    Operation operation,
    const GraphOptions& options,
    std::size_t maximum_retries,
    const MicrosoftGraphClient::SleepFunction& sleep,
    std::string_view description,
    const std::stop_token& stop_token = {},
    TransportErrorRetry transport_error_retry =
        TransportErrorRetry::disabled
) {
    std::size_t retries = 0;
    while (true) {
        if (stop_token.stop_requested()) {
            return cancelled_http_result();
        }
        auto response = operation();
        const bool transport_error = !response;
        if (transport_error &&
            (response.error().code != http::HttpErrorCode::transport ||
             transport_error_retry == TransportErrorRetry::disabled)) {
            return response;
        }
        if (!transport_error &&
            !retryable_status(response->status_code)) {
            return response;
        }
        if (retries >= maximum_retries) {
            if (transport_error) {
                throw std::runtime_error(
                    std::format(
                        "{} failed after {} retries: {}",
                        description,
                        retries,
                        response.error().message
                    )
                );
            }
            throw std::runtime_error(std::format(
                "{} failed with HTTP {} after {} retries",
                description,
                response->status_code,
                retries
            ));
        }

        const auto server_delay =
            transport_error ?
                std::optional<std::chrono::seconds>{} :
                retry_after(*response);
        if (server_delay &&
            *server_delay > options.maximum_throttle_delay) {
            throw std::runtime_error(
                "Microsoft Graph Retry-After exceeds the configured maximum "
                "throttle delay"
            );
        }
        const auto delay = server_delay.value_or(
            fallback_retry_delay(options, retries)
        );
        if (transport_error) {
            spdlog::warn(
                "{} failed: {}; retrying in {} seconds ({}/{})",
                description,
                response.error().message,
                delay.count(),
                retries + 1,
                maximum_retries
            );
        } else {
            spdlog::warn(
                "{} returned HTTP {}; retrying in {} seconds ({}/{})",
                description,
                response->status_code,
                delay.count(),
                retries + 1,
                maximum_retries
            );
        }
        if (wait_for_retry(delay, sleep, stop_token)) {
            return cancelled_http_result();
        }
        ++retries;
    }
}


[[nodiscard]] std::string item_remote_path(
    const Json& value,
    const std::string& name
);
[[nodiscard]] std::optional<std::string> file_system_last_modified(
    const Json& value,
    std::string_view description
);
[[nodiscard]] const Json* remote_item_facet(const Json& value);
[[nodiscard]] std::optional<std::string> authoritative_last_modified(
    const Json& value
);
[[nodiscard]] bool has_malware_facet(
    const Json& value,
    std::string_view description
);
[[nodiscard]] bool item_is_malware(const Json& value);
[[nodiscard]] std::optional<util::FileHash> item_content_hash(
    const Json& value
);
[[nodiscard]] std::string item_ctag(const Json& item);
[[nodiscard]] DriveInfo parse_drive_info(
    const Json& value,
    std::string_view description
);

enum class DriveItemKind {
    file_only,
    file_or_directory,
};

[[nodiscard]] RemoteItem parse_drive_item(
    const Json& json,
    std::string_view description,
    bool validate_content,
    DriveItemKind kind = DriveItemKind::file_only
);

}  // namespace onedrive::graph::client_detail
