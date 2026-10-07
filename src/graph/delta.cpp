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

DeltaResult MicrosoftGraphClient::list_delta(
    const std::optional<std::string>& delta_link,
    const DeltaProgress& progress
) const {
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
    spdlog::debug("Microsoft access token refreshed");
    cached_access_token_ = tokens->access_token;
    access_token_expires_at_ = tokens->expires_at;
    if (tokens->refresh_token != *refresh_token) {
        token_store_->save_refresh_token(tokens->refresh_token);
        spdlog::debug("Persisted rotated Microsoft refresh token");
    }

    std::string next_url;
    if (delta_link) {
        next_url = *delta_link;
        spdlog::info("Resuming Microsoft Graph delta query");
    } else {
        next_url = graph_drive_prefix(options_) + "/root/delta";
        spdlog::info("Starting initial Microsoft Graph delta query");
    }

    std::unordered_set<std::string> visited_urls;
    std::unordered_map<std::string, std::size_t> change_indexes;
    DeltaResult result;
    std::size_t page_number = 1;
    std::size_t scanned_item_count = 0;
    while (!next_url.empty()) {
        record_graph_page_url(
            next_url, options_.endpoint, visited_urls, "delta"
        );

        const auto response = perform_with_retries(
            [&] {
            spdlog::debug(
                "Requesting Microsoft Graph delta page {}",
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
            std::format("Microsoft Graph delta page {}", page_number)
        );
        if (!response) {
            throw std::runtime_error(
                "Microsoft Graph delta request failed: " +
                response.error().message
            );
        }

        const auto json = parse_graph_json(*response, "delta");
        if (delta_link && response->status_code == 410) {
            throw DeltaCursorInvalidError(
                "Microsoft Graph rejected the saved delta cursor: " +
                graph_error_message(json, response->status_code)
            );
        }
        require_successful_graph_response(json, response->status_code);

        try {
            const auto& values =
                require_graph_page_values(json, "delta");
            const std::size_t page_item_count = values.size();
            for (const auto& value : values) {
                RemoteItem item{};
                item.validate_content =
                    !options_.relaxed_download_validation;
                item.id = value.at("id").get<std::string>();
                item.deleted = value.contains("deleted");
                if (item.id.empty()) {
                    throw std::runtime_error(
                        "Microsoft Graph returned a delta item with an empty ID"
                    );
                }
                if (!item.deleted) {
                    item.name = value.at("name").get<std::string>();
                    if (const auto etag = value.find("eTag");
                        etag != value.end()) {
                        if (!etag->is_string()) {
                            throw std::runtime_error(
                                "Microsoft Graph returned a delta item with an "
                                "invalid eTag"
                            );
                        }
                        item.etag = etag->get<std::string>();
                    }
                    item.ctag = item_ctag(value);
                    item.directory = value.contains("folder");
                    item.root = value.contains("root");
                    if (!item.root) {
                        item.remote_path = item_remote_path(value, item.name);
                    }
                    if (const auto parent = value.find("parentReference");
                        parent != value.end() && parent->is_object()) {
                        if (const auto parent_id = parent->find("id");
                            parent_id != parent->end() && parent_id->is_string()) {
                            item.parent_id = parent_id->get<std::string>();
                        }
                    }
                    if (auto modified = authoritative_last_modified(value);
                        modified.has_value()) {
                        item.last_modified = std::move(modified.value());
                    }
                    if (const auto size = value.find("size");
                        size != value.end() && size->is_number_integer()) {
                        item.size = size->get<std::int64_t>();
                    }
                    item.content_hash = item_content_hash(value);
                    item.malware = item_is_malware(value);
                    if (item.name.empty() ||
                        (!item.root && item.remote_path.empty()) || item.size < 0) {
                        throw std::runtime_error(
                            "Microsoft Graph returned a delta item with invalid metadata"
                        );
                    }
                    if (!item.directory && item.last_modified.empty()) {
                        throw std::runtime_error(
                            "Microsoft Graph delta file is missing "
                            "fileSystemInfo.lastModifiedDateTime"
                        );
                    }
                }
                const auto [position, inserted] = change_indexes.emplace(
                    item.id,
                    result.changes.size()
                );
                if (inserted) {
                    result.changes.push_back(std::move(item));
                } else {
                    result.changes[position->second] = std::move(item);
                }
            }

            next_url = graph_next_link(json, "delta pagination")
                           .value_or(std::string{});
            if (next_url.empty()) {
                const auto final_link = json.find("@odata.deltaLink");
                if (final_link == json.end() || !final_link->is_string()) {
                    throw std::runtime_error(
                        "Microsoft Graph delta response did not contain a final "
                        "delta link"
                    );
                }
                result.delta_link = final_link->get<std::string>();
                require_graph_endpoint_url(
                    result.delta_link, options_.endpoint, "delta"
                );
                spdlog::debug(
                    "Received final Microsoft Graph delta cursor on page {}",
                    page_number
                );
            }
            spdlog::debug(
                "Received Microsoft Graph delta page {} with {} changes; {} total",
                page_number,
                page_item_count,
                result.changes.size()
            );
            scanned_item_count += page_item_count;
            const auto progress_state =
                next_url.empty() ?
                    util::ProgressState::completed :
                    util::ProgressState::ongoing;
            spdlog::trace(
                "Microsoft Graph delta progress: {} pages, {} items scanned ({})",
                page_number,
                scanned_item_count,
                progress_state == util::ProgressState::completed ?
                    "complete" :
                    "continuing"
            );
            if (progress) {
                progress(page_number, scanned_item_count, progress_state);
            }
            ++page_number;
        } catch (const Json::exception& error) {
            throw std::runtime_error(
                "Microsoft Graph delta response is missing required drive item "
                "data: " +
                std::string{error.what()}
            );
        }
    }

    spdlog::info(
        "Completed Microsoft Graph delta query: {} pages, {} changes",
        page_number - 1,
        result.changes.size()
    );
    return result;
}

}  // namespace onedrive::graph
