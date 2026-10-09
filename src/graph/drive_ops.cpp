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

DriveInfo client_detail::parse_drive_info(
    const Json& value,
    std::string_view description
) {
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

std::vector<DriveInfo> MicrosoftGraphClient::list_drives() const {
    std::vector<DriveInfo> drives;
    for (const auto& value : paged_graph_values(
             options_.endpoint +
                 "/me/drives?$select=id,name,driveType,webUrl,owner,quota",
             options_.endpoint,
             "drives",
             [&](const std::string& url) {
                 return parse_graph_json(
                     graph_get(url, "drives query"),
                     "drives"
                 );
             }
         )) {
        drives.push_back(parse_drive_info(value, "drive"));
    }
    return drives;
}

DriveInfo MicrosoftGraphClient::drive_info() const {
    return parse_drive_info(
        parse_graph_json(
            graph_get(
                graph_drive_prefix(options_) +
                    "?$select=id,name,driveType,webUrl,owner,quota",
                "drive information query"
            ),
            "drive information"
        ),
        "drive"
    );
}

std::vector<RemoteItem> MicrosoftGraphClient::list_root() const {
    const auto token = access_token();

    spdlog::info("Starting Microsoft Graph root directory listing");
    std::string next_url =
        graph_drive_prefix(options_) + "/root/children";
    std::unordered_set<std::string> visited_urls;
    std::vector<RemoteItem> items;
    std::size_t page_number = 1;

    while (!next_url.empty()) {
        record_graph_page_url(
            next_url, options_.endpoint, visited_urls, "pagination"
        );

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
                    "Authorization: Bearer " + token,
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
        require_successful_graph_response(json, response->status_code);

        try {
            const auto& values =
                require_graph_page_values(json, "root");
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

            next_url = graph_next_link(json, "pagination")
                           .value_or(std::string{});
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
    return item_by_path(remote_path, {});
}

RemoteItem MicrosoftGraphClient::item_by_path(
    const std::string& remote_path,
    std::stop_token stop_token
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
                .stop_token = stop_token,
            });
        },
        options_,
        options_.maximum_throttle_retries,
        sleep_,
        "Microsoft Graph path lookup",
        stop_token
    );
    if (!response) {
        if (stop_token.stop_requested() ||
            response.error().code == http::HttpErrorCode::cancelled) {
            throw RequestCancelledError{
                "Microsoft Graph path lookup was cancelled"
            };
        }
        throw std::runtime_error(
            "Microsoft Graph path lookup failed: " + response.error().message
        );
    }

    const auto json = parse_graph_json(*response, "path lookup");
    require_successful_graph_response(json, response->status_code);

    return parse_drive_item(
        json,
        "path lookup",
        options_.relaxed_download_validation ?
            ContentValidation::relaxed :
            ContentValidation::strict,
        DriveItemKind::file_or_directory
    );
}

RemoteItem MicrosoftGraphClient::create_directory(
    const std::string& remote_path
) const {
    return create_directory(remote_path, {});
}

RemoteItem MicrosoftGraphClient::create_directory(
    const std::string& remote_path,
    std::stop_token stop_token
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
                .stop_token = stop_token,
            });
        },
        options_,
        options_.maximum_throttle_retries,
        sleep_,
        "Microsoft Graph directory creation",
        stop_token
    );
    if (!response) {
        if (stop_token.stop_requested() ||
            response.error().code == http::HttpErrorCode::cancelled) {
            throw RequestCancelledError{
                "Microsoft Graph directory creation was cancelled"
            };
        }
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
        options_.relaxed_download_validation ?
            ContentValidation::relaxed :
            ContentValidation::strict,
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
    delete_item(remote_id, expected_etag, {});
}

void MicrosoftGraphClient::delete_item(
    const std::string& remote_id,
    const std::string& expected_etag,
    std::stop_token stop_token
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
                .stop_token = stop_token,
            });
        },
        options_,
        options_.maximum_throttle_retries,
        sleep_,
        "Microsoft Graph item deletion",
        stop_token
    );
    if (!response) {
        if (stop_token.stop_requested() ||
            response.error().code == http::HttpErrorCode::cancelled) {
            throw RequestCancelledError{
                "Microsoft Graph item deletion was cancelled"
            };
        }
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
    require_successful_graph_response(json, response->status_code);
}

RemoteItem MicrosoftGraphClient::move_item(
    const std::string& remote_id,
    const std::string& expected_etag,
    const std::string& destination_path
) const {
    return move_item(remote_id, expected_etag, destination_path, {});
}

RemoteItem MicrosoftGraphClient::move_item(
    const std::string& remote_id,
    const std::string& expected_etag,
    const std::string& destination_path,
    std::stop_token stop_token
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
                .stop_token = stop_token,
            });
        },
        options_,
        options_.maximum_throttle_retries,
        sleep_,
        "Microsoft Graph item move",
        stop_token
    );
    if (!response) {
        if (stop_token.stop_requested() ||
            response.error().code == http::HttpErrorCode::cancelled) {
            throw RequestCancelledError{
                "Microsoft Graph item move was cancelled"
            };
        }
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
    require_successful_graph_response(json, response->status_code);
    return parse_drive_item(
        json,
        "item move response",
        options_.relaxed_download_validation ?
            ContentValidation::relaxed :
            ContentValidation::strict,
        DriveItemKind::file_or_directory
    );
}

}  // namespace onedrive::graph
