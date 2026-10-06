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
    using client_detail::parse_graph_json;
    using client_detail::successful_status;

    const auto response = transport_->perform(
        http::HttpRequest{
            .method = http::HttpMethod::get,
            .url =
                graph_drive_prefix(options_) + "/root/subscriptions/socketIo",
            .headers =
                {
                    "Accept: application/json",
                    "Authorization: Bearer " + access_token(),
                },
            .body = {},
            .connect_timeout = std::chrono::seconds{30},
            .operation_timeout = std::chrono::seconds{60},
            .maximum_response_size = std::size_t{1024} * 1024U,
            .stop_token = {},
        }
    );
    if (!response) {
        throw NotificationChannelError{
            false,
            "Microsoft Graph notification channel request failed: " +
                response.error().message,
        };
    }
    const auto json = parse_graph_json(*response, "notification channel");
    if (!successful_status(response->status_code)) {
        if (response->status_code == 401) {
            invalidate_access_token();
        }
        throw NotificationChannelError{
            response->status_code == 401,
            std::format(
                "Microsoft Graph notification channel request failed with "
                "HTTP {}",
                response->status_code
            ),
        };
    }
    try {
        const auto url = json.at("notificationUrl").get<std::string>();
        const auto expiration =
            json.at("expirationDateTime").get<std::string>();
        if (url.empty()) {
            throw std::runtime_error(
                "Microsoft Graph returned an empty notification URL"
            );
        }
        return {
            .notification_url = url,
            .expires_at = std::chrono::time_point_cast<
                std::chrono::system_clock::duration>(
                util::parse_remote_modified_time(expiration)
            ),
        };
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(
            "Microsoft Graph notification channel response is invalid: " +
            std::string{error.what()}
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
