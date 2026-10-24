#include "onedrive/graph/graph_client.hpp"

#include "graph/support.hpp"
#include "onedrive/http/http_client.hpp"
#include "onedrive/util/remote_time.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <format>
#include <stdexcept>
#include <string>

namespace onedrive::graph {

NotificationChannel MicrosoftGraphClient::notification_channel() const {
    using client_detail::graph_drive_prefix;
    using client_detail::successful_status;

    const auto response = transport_->perform(
        http::HttpRequest{
            .method = http::HttpMethod::get,
            .url =
                graph_drive_prefix(options_) + "/root/subscriptions/socketIo",
            .headers =
                {
                    "Accept: application/json",
                    "Content-Type: application/json",
                    "Authorization: Bearer " + access_token(),
                },
            .body = {},
            .connect_timeout = options_.notification_request_timeout,
            .operation_timeout = options_.notification_request_timeout,
            .maximum_response_size = client_detail::maximum_json_response_bytes,
            .stop_token = {},
        }
    );
    if (!response) {
        throw NotificationChannelError{
            NotificationFailureKind::transient,
            "Microsoft Graph notification channel request failed: " +
                response.error().message,
        };
    }
    if (!successful_status(response->status_code)) {
        if (response->status_code == http::status::unauthorized) {
            invalidate_access_token();
        }
        auto kind = NotificationFailureKind::transient;
        switch (response->status_code) {
        case http::status::unauthorized:
            kind = NotificationFailureKind::unauthorized;
            break;
        case http::status::forbidden:
        case http::status::not_found:
        case http::status::method_not_allowed:
        case http::status::gone:
        case http::status::not_implemented:
            kind = NotificationFailureKind::unavailable;
            break;
        default:
            break;
        }
        throw NotificationChannelError{
            kind,
            std::format(
                "Microsoft Graph notification channel request failed with "
                "HTTP {}",
                response->status_code
            ),
        };
    }
    try {
        const auto json = nlohmann::json::parse(response->body);
        const auto url = json.at("notificationUrl").get<std::string>();
        if (url.empty()) {
            throw NotificationChannelError(
                NotificationFailureKind::transient,
                "Microsoft Graph returned an empty notification URL"
            );
        }
        NotificationChannel channel{
            .notification_url = url,
        };
        if (const auto expiration = json.find("expirationDateTime");
            expiration != json.end()) {
            const auto timestamp = expiration->get<std::string>();
            try {
                channel.expires_at = std::chrono::time_point_cast<
                    std::chrono::system_clock::duration>(
                    util::parse_remote_modified_time(timestamp)
                );
            } catch (const std::runtime_error&) {
                throw NotificationChannelError(
                    NotificationFailureKind::transient,
                    "Microsoft Graph notification channel expiration is invalid"
                );
            }
        }
        return channel;
    } catch (const nlohmann::json::exception&) {
        throw NotificationChannelError(
            NotificationFailureKind::transient,
            "Microsoft Graph notification channel response is invalid"
        );
    }
}

void MicrosoftGraphClient::refresh_access_token() const {
    invalidate_access_token();
    static_cast<void>(access_token());
}

void MicrosoftGraphClient::invalidate_access_token() const {
    const std::scoped_lock lock{access_token_mutex_};
    cached_access_token_.clear();
    access_token_expires_at_ = {};
}

} // namespace onedrive::graph
