#include "onedrive/graph/graph_client.hpp"
#include "graph/support.hpp"

#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/http/http_client.hpp"
#include "onedrive/http/transfer_rate_limiter.hpp"
#include "onedrive/util/remote_time.hpp"
#include "util/ascii.hpp"
#include "util/typestate.hpp"
#include "util/uri.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace onedrive::graph {

using namespace client_detail;
using onedrive::util::percent_encode_uri_component;

namespace {

std::uint64_t quota_value(
    const Json& quota,
    std::string_view name,
    std::string_view description
) {
    try {
        const auto value = quota.at(name).get<std::int64_t>();
        if (value < 0) {
            throw std::runtime_error(
                "Microsoft Graph returned a negative " +
                std::string{description} + " quota value"
            );
        }
        return static_cast<std::uint64_t>(value);
    } catch (const Json::exception& error) {
        throw std::runtime_error(
            "Microsoft Graph " + std::string{description} +
            " quota is missing required data: " + error.what()
        );
    }
}

DriveInfo parse_drive_info(const Json& value, std::string_view description) {
    try {
        DriveInfo drive{
            .id = value.at("id").get<std::string>(),
            .name = value.at("name").get<std::string>(),
            .type = value.value("driveType", std::string{}),
            .web_url = value.value("webUrl", std::string{}),
            .owner = {},
            .quota = std::nullopt,
        };
        if (const auto owner = value.find("owner");
            owner != value.end() && owner->is_object()) {
            for (const auto* kind : {"user", "group"}) {
                if (const auto identity = owner->find(kind);
                    identity != owner->end() && identity->is_object()) {
                    drive.owner =
                        identity->value("displayName", std::string{});
                    break;
                }
            }
        }
        if (const auto quota = value.find("quota");
            quota != value.end()) {
            if (!quota->is_object()) {
                throw std::runtime_error(
                    "Microsoft Graph returned an invalid " +
                    std::string{description} + " quota"
                );
            }
            drive.quota = DriveQuota{
                .total = quota_value(*quota, "total", description),
                .used = quota_value(*quota, "used", description),
                .remaining = quota_value(*quota, "remaining", description),
                .deleted = quota_value(*quota, "deleted", description),
                .state = quota->value("state", std::string{}),
            };
        }
        if (drive.id.empty() || drive.name.empty()) {
            throw std::runtime_error(
                "Microsoft Graph returned incomplete " +
                std::string{description} + " metadata"
            );
        }
        return drive;
    } catch (const Json::exception& error) {
        throw std::runtime_error(
            "Microsoft Graph " + std::string{description} +
            " is missing required data: " + error.what()
        );
    }
}

}  // namespace

std::vector<DriveInfo> MicrosoftGraphClient::list_drives() const {
    std::string next_url =
        options_.endpoint +
        "/me/drives?$select=id,name,driveType,webUrl,owner,quota";
    const std::string allowed_url_prefix = options_.endpoint + "/";
    std::unordered_set<std::string> visited_urls;
    std::vector<DriveInfo> drives;
    while (!next_url.empty()) {
        if (!next_url.starts_with(allowed_url_prefix) ||
            !visited_urls.insert(next_url).second) {
            throw std::runtime_error(
                "Microsoft Graph returned an invalid drives pagination URL"
            );
        }
        const auto response = perform_with_retries(
            [&] {
                return transport_->perform(http::HttpRequest{
                    .method = http::HttpMethod::get,
                    .url = next_url,
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
            "Microsoft Graph drives query"
        );
        if (!response) {
            throw std::runtime_error(
                "Microsoft Graph drives query failed: " +
                response.error().message
            );
        }
        const auto json = parse_graph_json(*response, "drives");
        if (!successful_status(response->status_code)) {
            throw std::runtime_error(
                graph_error_message(json, response->status_code)
            );
        }
        try {
            const auto& values = json.at("value");
            if (!values.is_array()) {
                throw std::runtime_error(
                    "Microsoft Graph drives response field 'value' is not an "
                    "array"
                );
            }
            for (const auto& value : values) {
                drives.push_back(parse_drive_info(value, "drive"));
            }
            next_url.clear();
            if (const auto next = json.find("@odata.nextLink");
                next != json.end()) {
                if (!next->is_string()) {
                    throw std::runtime_error(
                        "Microsoft Graph returned an invalid drives "
                        "pagination URL"
                    );
                }
                next_url = next->get<std::string>();
            }
        } catch (const Json::exception& error) {
            throw std::runtime_error(
                "Microsoft Graph drives response is missing required data: " +
                std::string{error.what()}
            );
        }
    }
    return drives;
}

DriveInfo MicrosoftGraphClient::drive_info() const {
    const auto response = perform_with_retries(
        [&] {
            return transport_->perform(http::HttpRequest{
                .method = http::HttpMethod::get,
                .url =
                    graph_drive_prefix(options_) +
                    "?$select=id,name,driveType,webUrl,owner,quota",
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
        "Microsoft Graph drive information query"
    );
    if (!response) {
        throw std::runtime_error(
            "Microsoft Graph drive information query failed: " +
            response.error().message
        );
    }
    const auto json = parse_graph_json(*response, "drive information");
    if (!successful_status(response->status_code)) {
        throw std::runtime_error(
            graph_error_message(json, response->status_code)
        );
    }
    return parse_drive_info(json, "drive");
}

std::vector<RemoteItem> MicrosoftGraphClient::list_root() const {
    spdlog::debug("Loading saved Microsoft authentication");
    const auto refresh_token = token_store_->load_refresh_token();
    if (!refresh_token) {
        throw std::runtime_error(
            "no saved Microsoft authentication is available; run 'onedrive-cpp auth'"
        );
    }

    spdlog::debug("Refreshing Microsoft access token");
    auto tokens = auth_client_->refresh_access_token(*refresh_token);
    if (!tokens) {
        throw std::runtime_error(
            "cannot refresh Microsoft access token: " + tokens.error().message
        );
    }
    spdlog::debug("Microsoft access token refreshed");
    cached_access_token_ = tokens->access_token;
    access_token_expires_at_ = tokens->expires_at;
    if (tokens->refresh_token != *refresh_token) {
        token_store_->save_refresh_token(tokens->refresh_token);
        spdlog::debug("Persisted rotated Microsoft refresh token");
    }

    spdlog::info("Starting Microsoft Graph root directory listing");
    std::string next_url =
        graph_drive_prefix(options_) + "/root/children";
    const std::string allowed_url_prefix = options_.endpoint + "/";
    std::unordered_set<std::string> visited_urls;
    std::vector<RemoteItem> items;
    std::size_t page_number = 1;

    while (!next_url.empty()) {
        if (!next_url.starts_with(allowed_url_prefix)) {
            throw std::runtime_error(
                "Microsoft Graph returned a pagination URL outside its endpoint"
            );
        }
        if (!visited_urls.insert(next_url).second) {
            throw std::runtime_error(
                "Microsoft Graph returned a repeated pagination URL"
            );
        }

        const auto response = perform_with_retries(
            [&] {
            spdlog::debug(
                "Requesting Microsoft Graph root page {}",
                page_number
            );
            return transport_->perform(http::HttpRequest{
                .method = http::HttpMethod::get,
                .url = next_url,
                .headers = {
                    "Accept: application/json",
                    "Authorization: Bearer " + tokens->access_token,
                },
                .body = {},
                .stop_token = {},
            });
            },
            options_,
            options_.maximum_throttle_retries,
            sleep_,
            std::format("Microsoft Graph root page {}", page_number)
        );
        if (!response) {
            throw std::runtime_error(
                "Microsoft Graph request failed: " + response.error().message
            );
        }

        const auto json = parse_graph_json(*response);
        if (!successful_status(response->status_code)) {
            throw std::runtime_error(
                graph_error_message(json, response->status_code)
            );
        }

        try {
            const auto& values = json.at("value");
            if (!values.is_array()) {
                throw std::runtime_error(
                    "Microsoft Graph response field 'value' is not an array"
                );
            }
            const std::size_t page_item_count = values.size();
            for (const auto& value : values) {
                RemoteItem item{
                    .id = value.at("id").get<std::string>(),
                    .name = value.at("name").get<std::string>(),
                    .etag = value.at("eTag").get<std::string>(),
                    .ctag = item_ctag(value),
                    .parent_id = {},
                    .remote_path = {},
                    .last_modified = {},
                    .size = 0,
                    .directory = value.contains("folder"),
                    .deleted = false,
                    .root = false,
                    .content_hash = std::nullopt,
                    .validate_content =
                        !options_.relaxed_download_validation,
                };
                if (item.id.empty() || item.name.empty() || item.etag.empty()) {
                    throw std::runtime_error(
                        "Microsoft Graph returned a drive item with empty metadata"
                    );
                }
                item.content_hash = item_content_hash(value);
                item.malware = item_is_malware(value);
                items.push_back(std::move(item));
            }

            next_url.clear();
            if (const auto next = json.find("@odata.nextLink");
                next != json.end()) {
                if (!next->is_string()) {
                    throw std::runtime_error(
                        "Microsoft Graph returned an invalid pagination URL"
                    );
                }
                next_url = next->get<std::string>();
            }
            spdlog::debug(
                "Received Microsoft Graph root page {} with {} items; {} total",
                page_number,
                page_item_count,
                items.size()
            );
            ++page_number;
        } catch (const Json::exception& error) {
            throw std::runtime_error(
                "Microsoft Graph response is missing required drive item data: " +
                std::string{error.what()}
            );
        }
    }

    spdlog::info(
        "Completed Microsoft Graph root directory listing: {} pages, {} items",
        page_number - 1,
        items.size()
    );
    return items;
}

RemoteItem MicrosoftGraphClient::item_by_path(
    const std::string& remote_path
) const {
    const auto encoded_path = percent_encode_remote_path(remote_path);
    const std::string url =
        graph_drive_prefix(options_) + "/root:/" + encoded_path +
        "?$select=id,name,eTag,cTag,size,fileSystemInfo,parentReference,file,folder,"
        "deleted,malware,remoteItem";
    const auto response = perform_with_retries(
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
        "Microsoft Graph path lookup"
    );
    if (!response) {
        throw std::runtime_error(
            "Microsoft Graph path lookup failed: " + response.error().message
        );
    }

    const auto json = parse_graph_json(*response, "path lookup");
    if (!successful_status(response->status_code)) {
        throw std::runtime_error(
            graph_error_message(json, response->status_code)
        );
    }

    return parse_drive_item(
        json,
        "path lookup",
        !options_.relaxed_download_validation,
        DriveItemKind::file_or_directory
    );
}

RemoteItem MicrosoftGraphClient::create_directory(
    const std::string& remote_path
) const {
    const auto separator = remote_path.rfind('/');
    const auto name = separator == std::string::npos ?
        remote_path :
        remote_path.substr(separator + 1);
    if (name.empty()) {
        throw std::invalid_argument(
            "cannot create a remote directory without a name"
        );
    }
    const auto drive_prefix = graph_drive_prefix(options_);
    const auto parent = separator == std::string::npos ?
        std::string{} :
        remote_path.substr(0, separator);
    const auto url = parent.empty() ?
        drive_prefix + "/root/children" :
        drive_prefix + "/root:/" +
            percent_encode_remote_path(parent) + ":/children";
    const auto body = Json{
        {"name", name},
        {"folder", Json::object()},
        {"@microsoft.graph.conflictBehavior", "fail"},
    }.dump();
    const auto& transfer = options_.upload_transport.transfer;
    const auto response = perform_with_retries(
        [&] {
            return transport_->perform(http::HttpRequest{
                .method = http::HttpMethod::post,
                .url = url,
                .headers = {
                    "Accept: application/json",
                    "Authorization: Bearer " + access_token(),
                    "Content-Type: application/json",
                },
                .body = body,
                .connect_timeout = transfer.connect_timeout,
                .operation_timeout = transfer.operation_timeout,
                .low_speed_timeout = transfer.low_speed_timeout,
                .low_speed_limit_bytes_per_second =
                    transfer.low_speed_limit_bytes_per_second,
                .maximum_send_speed_bytes_per_second =
                    effective_upload_rate(options_.upload_transport),
                .http_version = transfer.http_version,
                .ip_version = transfer.ip_version,
                .maximum_response_size = std::size_t{1024} * 1024U,
                .stop_token = {},
            });
        },
        options_,
        options_.maximum_throttle_retries,
        sleep_,
        "Microsoft Graph directory creation"
    );
    if (!response) {
        throw std::runtime_error(
            "Microsoft Graph directory creation failed: " +
            response.error().message
        );
    }
    const auto json = parse_graph_json(*response, "directory creation");
    if (response->status_code == 409 || response->status_code == 412) {
        throw UploadConflictError(
            graph_error_message(json, response->status_code)
        );
    }
    if (!successful_status(response->status_code)) {
        throw_upload_response_error(json, response->status_code);
    }
    auto item = parse_drive_item(
        json,
        "directory creation response",
        !options_.relaxed_download_validation,
        DriveItemKind::file_or_directory
    );
    if (!item.directory) {
        throw std::runtime_error(
            "Microsoft Graph directory creation returned a file"
        );
    }
    return item;
}

void MicrosoftGraphClient::delete_item(
    const std::string& remote_id,
    const std::string& expected_etag
) const {
    if (remote_id.empty() || expected_etag.empty() ||
        expected_etag.find_first_of("\r\n") != std::string::npos) {
        throw std::invalid_argument(
            "remote deletion requires an item ID and a valid eTag"
        );
    }
    const auto drive_prefix = graph_drive_prefix(options_);
    const auto& transfer = options_.upload_transport.transfer;
    const auto response = perform_with_retries(
        [&] {
            return transport_->perform(http::HttpRequest{
                .method = http::HttpMethod::delete_,
                .url = drive_prefix + "/items/" +
                    percent_encode_uri_component(remote_id),
                .headers = {
                    "Accept: application/json",
                    "Authorization: Bearer " + access_token(),
                    "If-Match: " + expected_etag,
                },
                .body = {},
                .connect_timeout = transfer.connect_timeout,
                .operation_timeout = transfer.operation_timeout,
                .low_speed_timeout = transfer.low_speed_timeout,
                .low_speed_limit_bytes_per_second =
                    transfer.low_speed_limit_bytes_per_second,
                .maximum_send_speed_bytes_per_second =
                    effective_upload_rate(options_.upload_transport),
                .http_version = transfer.http_version,
                .ip_version = transfer.ip_version,
                .maximum_response_size = std::size_t{1024} * 1024U,
                .stop_token = {},
            });
        },
        options_,
        options_.maximum_throttle_retries,
        sleep_,
        "Microsoft Graph item deletion"
    );
    if (!response) {
        throw std::runtime_error(
            "Microsoft Graph item deletion failed: " +
            response.error().message
        );
    }
    if (response->status_code == 404) {
        return;
    }
    if (successful_status(response->status_code)) {
        return;
    }
    const auto json = parse_graph_json(*response, "item deletion");
    if (response->status_code == 409 || response->status_code == 412) {
        throw UploadConflictError(
            graph_error_message(json, response->status_code)
        );
    }
    throw std::runtime_error(
        graph_error_message(json, response->status_code)
    );
}

RemoteItem MicrosoftGraphClient::move_item(
    const std::string& remote_id,
    const std::string& expected_etag,
    const std::string& destination_path
) const {
    const auto separator = destination_path.rfind('/');
    const auto name = separator == std::string::npos ?
        destination_path :
        destination_path.substr(separator + 1);
    if (remote_id.empty() || expected_etag.empty() || name.empty() ||
        expected_etag.find_first_of("\r\n") != std::string::npos) {
        throw std::invalid_argument(
            "remote move requires an item ID, valid eTag, and destination"
        );
    }
    const auto parent = separator == std::string::npos ?
        std::string{} :
        destination_path.substr(0, separator);
    const auto drive_prefix = graph_drive_prefix(options_);
    const auto body = Json{
        {"name", name},
        {"parentReference", {
            {"path", parent.empty() ?
                "/drive/root:" :
                "/drive/root:/" + parent},
        }},
    }.dump();
    const auto& transfer = options_.upload_transport.transfer;
    const auto response = perform_with_retries(
        [&] {
            return transport_->perform(http::HttpRequest{
                .method = http::HttpMethod::patch,
                .url = drive_prefix + "/items/" +
                    percent_encode_uri_component(remote_id),
                .headers = {
                    "Accept: application/json",
                    "Authorization: Bearer " + access_token(),
                    "Content-Type: application/json",
                    "If-Match: " + expected_etag,
                },
                .body = body,
                .connect_timeout = transfer.connect_timeout,
                .operation_timeout = transfer.operation_timeout,
                .low_speed_timeout = transfer.low_speed_timeout,
                .low_speed_limit_bytes_per_second =
                    transfer.low_speed_limit_bytes_per_second,
                .maximum_send_speed_bytes_per_second =
                    effective_upload_rate(options_.upload_transport),
                .http_version = transfer.http_version,
                .ip_version = transfer.ip_version,
                .maximum_response_size = std::size_t{1024} * 1024U,
                .stop_token = {},
            });
        },
        options_,
        options_.maximum_throttle_retries,
        sleep_,
        "Microsoft Graph item move"
    );
    if (!response) {
        throw std::runtime_error(
            "Microsoft Graph item move failed: " +
            response.error().message
        );
    }
    const auto json = parse_graph_json(*response, "item move");
    if (response->status_code == 409 || response->status_code == 412) {
        throw UploadConflictError(
            graph_error_message(json, response->status_code)
        );
    }
    if (!successful_status(response->status_code)) {
        throw std::runtime_error(
            graph_error_message(json, response->status_code)
        );
    }
    return parse_drive_item(
        json,
        "item move response",
        !options_.relaxed_download_validation,
        DriveItemKind::file_or_directory
    );
}

}  // namespace onedrive::graph
