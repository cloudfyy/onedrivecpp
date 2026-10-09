#include "onedrive/graph/graph_client.hpp"
#include "graph/support.hpp"

#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "util/ascii.hpp"
#include "util/typestate.hpp"
#include "util/uri.hpp"
#include "onedrive/http/transfer_rate_limiter.hpp"
#include "onedrive/http/http_client.hpp"
#include "onedrive/util/remote_time.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <fstream>

#include <format>
#include <limits>
#include <optional>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace onedrive::graph {
namespace {

using client_detail::graph_drive_prefix;
using client_detail::graph_error_message;
using client_detail::header_value;
using client_detail::Json;
using client_detail::normalized_endpoint;
using client_detail::parse_graph_json;
using client_detail::successful_status;

constexpr std::uint64_t upload_chunk_quantum =
    std::uint64_t{320} * 1024U;
constexpr std::uint64_t maximum_upload_chunk_size =
    std::uint64_t{60} * 1024U * 1024U;

}  // namespace

MicrosoftGraphClient::MicrosoftGraphClient(
    std::unique_ptr<http::HttpTransport> transport,
    std::unique_ptr<auth::TokenStore> token_store,
    auth::DeviceAuthOptions auth_options,
    GraphOptions options,
    SleepFunction sleep
)
    : transport_{std::move(transport)},
      token_store_{std::move(token_store)},
      options_{std::move(options)},
      sleep_{
          sleep ? std::move(sleep) :
                  SleepFunction{[](std::chrono::seconds duration) {
                      std::this_thread::sleep_for(duration);
                  }}
      } {
    options_.endpoint = normalized_endpoint(std::move(options_.endpoint));
    if (!transport_ || !token_store_) {
        throw std::invalid_argument(
            "Microsoft Graph client requires HTTP transport and token store"
        );
    }
    if (!options_.endpoint.starts_with("https://") || options_.drive_id.empty()) {
        throw std::invalid_argument(
            "Microsoft Graph client requires an HTTPS endpoint and drive ID"
        );
    }
    if (options_.initial_throttle_delay < std::chrono::seconds::zero() ||
        options_.maximum_throttle_delay < options_.initial_throttle_delay ||
        options_.notification_request_timeout <=
            std::chrono::seconds::zero()) {
        throw std::invalid_argument(
            "Microsoft Graph client requires valid retry delays and "
            "notification timeout"
        );
    }
    const auto& download_transport = options_.download_transport;
    const auto& transfer = download_transport.transfer;
    if (options_.download_chunk_threshold_bytes == 0 ||
        options_.download_checkpoint_interval_bytes == 0 ||
        transfer.connect_timeout <=
            std::chrono::seconds::zero() ||
        transfer.operation_timeout <=
            std::chrono::seconds::zero() ||
        transfer.low_speed_timeout < std::chrono::seconds::zero() ||
        transfer.low_speed_limit_bytes_per_second == 0) {
        throw std::invalid_argument(
            "Microsoft Graph client requires valid download transport options"
        );
    }
    const auto& upload_transport = options_.upload_transport;
    const auto& upload_transfer = upload_transport.transfer;
    if (options_.simple_upload_threshold_bytes == 0 ||
        options_.upload_chunk_size_bytes == 0 ||
        options_.upload_chunk_size_bytes % upload_chunk_quantum != 0 ||
        options_.upload_chunk_size_bytes >= maximum_upload_chunk_size ||
        upload_transfer.connect_timeout <=
            std::chrono::seconds::zero() ||
        upload_transfer.operation_timeout <=
            std::chrono::seconds::zero() ||
        upload_transfer.low_speed_timeout < std::chrono::seconds::zero() ||
        upload_transfer.low_speed_limit_bytes_per_second == 0) {
        throw std::invalid_argument(
            "Microsoft Graph client requires valid upload transport options"
        );
    }
    auth_client_ =
        std::make_unique<auth::DeviceAuthClient>(
            transport_.get(),
            std::move(auth_options)
        );
    if (download_transport.
            maximum_total_receive_speed_bytes_per_second != 0) {
        download_rate_limiter_ =
            std::make_unique<http::TransferRateLimiter>(
                download_transport.
                    maximum_total_receive_speed_bytes_per_second
            );
    }
    if (upload_transport.maximum_total_send_speed_bytes_per_second != 0) {
        upload_rate_limiter_ =
            std::make_unique<http::TransferRateLimiter>(
                upload_transport.maximum_total_send_speed_bytes_per_second
            );
    }
}

MicrosoftGraphClient::~MicrosoftGraphClient() = default;

http::HttpResponse MicrosoftGraphClient::graph_get(
    const std::string& url,
    std::string_view description
) const {
    auto response = client_detail::perform_with_retries(
        [&] {
            return transport_->perform(http::HttpRequest{
                .method = http::HttpMethod::get,
                .url = url,
                .headers = {
                    "Accept: application/json",
                    "Authorization: Bearer " + access_token(),
                },
                .body = {},
                .stop_token = {},
            });
        },
        options_,
        options_.maximum_throttle_retries,
        sleep_,
        description
    );
    if (!response) {
        throw std::runtime_error(
            "Microsoft Graph " + std::string{description} +
            " failed: " + response.error().message
        );
    }
    if (!client_detail::successful_status(response->status_code)) {
        const auto document =
            client_detail::parse_graph_json(*response, description);
        client_detail::require_successful_graph_response(
            document, response->status_code
        );
    }
    return *std::move(response);
}

account::DriveIdentity fetch_drive_identity(
    const http::HttpTransport& transport,
    std::string_view access_token,
    GraphOptions options,
    std::stop_token stop_token
) {
    options.endpoint = normalized_endpoint(std::move(options.endpoint));
    if (access_token.empty() || !options.endpoint.starts_with("https://") ||
        options.drive_id.empty()) {
        throw std::invalid_argument(
            "Microsoft Graph identity query requires a token, HTTPS endpoint, "
            "and drive ID"
        );
    }
    const auto request_json = [&](const std::string& url, std::string_view name) {
        if (stop_token.stop_requested()) {
            throw std::runtime_error{"Microsoft Graph identity query cancelled"};
        }
        const auto response = transport.perform(http::HttpRequest{
            .method = http::HttpMethod::get,
            .url = url,
            .headers = {
                "Accept: application/json",
                "Authorization: Bearer " + std::string{access_token},
            },
            .body = {},
            .connect_timeout = std::chrono::seconds{30},
            .operation_timeout = std::chrono::seconds{60},
            .maximum_response_size = std::size_t{1024} * 1024U,
            .stop_token = stop_token,
        });
        if (!response) {
            throw std::runtime_error(
                "Microsoft Graph " + std::string{name} +
                " request failed: " + response.error().message
            );
        }
        const auto json = parse_graph_json(*response, name);
        client_detail::require_successful_graph_response(
            json,
            response->status_code,
            "Microsoft Graph " + std::string{name} + " query failed"
        );
        return json;
    };

    const auto user = request_json(
        options.endpoint + "/me?$select=id,displayName",
        "user identity"
    );
    const auto drive_url =
        graph_drive_prefix(options) + "?$select=id,name";
    const auto drive = request_json(drive_url, "drive identity");

    account::DriveIdentity identity;
    identity.configured_drive_id = options.drive_id;
    try {
        identity.user_id = user.at("id").get<std::string>();
        identity.user_display_name =
            user.at("displayName").get<std::string>();
        identity.drive_id = drive.at("id").get<std::string>();
        identity.drive_name = drive.value("name", std::string{"OneDrive"});
    } catch (const Json::exception& error) {
        throw std::runtime_error(
            "Microsoft Graph identity response is missing required data: " +
            std::string{error.what()}
        );
    }
    if (identity.user_id.empty() || identity.user_display_name.empty() ||
        identity.drive_id.empty()) {
        throw std::runtime_error(
            "Microsoft Graph returned an incomplete user or drive identity"
        );
    }
    if (identity.drive_name.empty()) {
        identity.drive_name = "OneDrive";
    }

    if (stop_token.stop_requested()) {
        throw std::runtime_error{"Microsoft Graph identity query cancelled"};
    }
    const auto photo = transport.perform(http::HttpRequest{
        .method = http::HttpMethod::get,
        .url = options.endpoint + "/me/photo/$value",
        .headers = {
            "Accept: image/*",
            "Authorization: Bearer " + std::string{access_token},
        },
        .body = {},
        .connect_timeout = std::chrono::seconds{30},
        .operation_timeout = std::chrono::seconds{60},
        .maximum_response_size = std::size_t{8} * 1024U * 1024U,
        .stop_token = stop_token,
    });
    if (!photo) {
        throw std::runtime_error(
            "Microsoft Graph profile photo request failed: " +
            photo.error().message
        );
    }
    if (photo->status_code == 404) {
        spdlog::debug("Microsoft account has no profile photo");
    } else if (successful_status(photo->status_code)) {
        const auto content_type =
            header_value(*photo, "Content-Type").value_or("image/jpeg");
        if (!content_type.starts_with("image/")) {
            throw std::runtime_error(
                "Microsoft Graph profile photo has an invalid content type"
            );
        }
        identity.photo = account::ProfilePhoto{
            .content_type = content_type,
            .bytes = std::vector<std::uint8_t>{
                photo->body.begin(),
                photo->body.end()
            },
        };
    } else {
        throw std::runtime_error(
            std::format(
                "Microsoft Graph profile photo query failed with HTTP {}",
                photo->status_code
            )
        );
    }
    return identity;
}

std::string MicrosoftGraphClient::access_token() const {
    const std::scoped_lock lock{access_token_mutex_};
    constexpr auto expiry_margin = std::chrono::minutes{1};
    if (!cached_access_token_.empty() &&
        access_token_expires_at_ > std::chrono::system_clock::now() + expiry_margin) {
        return cached_access_token_;
    }

    spdlog::debug("Loading saved Microsoft authentication");
    const auto refresh_token = token_store_->load_refresh_token();
    if (!refresh_token) {
        throw std::runtime_error(
            "no saved Microsoft authentication is available; run "
            "'onedrive-cpp account login'"
        );
    }
    spdlog::debug("Refreshing Microsoft access token");
    auto tokens = auth_client_->refresh_access_token(*refresh_token);
    if (!tokens) {
        throw std::runtime_error(
            "cannot refresh Microsoft access token: " + tokens.error().message
        );
    }
    if (tokens->refresh_token != *refresh_token) {
        token_store_->save_refresh_token(tokens->refresh_token);
        spdlog::debug("Persisted rotated Microsoft refresh token");
    }
    cached_access_token_ = tokens->access_token;
    access_token_expires_at_ = tokens->expires_at;
    spdlog::debug("Microsoft access token refreshed");
    return cached_access_token_;
}

account::DriveIdentity MicrosoftGraphClient::drive_identity() const {
    return fetch_drive_identity(*transport_, access_token(), options_);
}

}  // namespace onedrive::graph
