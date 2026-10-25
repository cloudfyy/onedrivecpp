#include "onedrive/graph/graph_client.hpp"
#include "graph/support.hpp"
#include "graph/delta_changes.hpp"
#include "graph/delta_hierarchy.hpp"

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
#include <span>
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
    const DeltaProgress& progress,
    std::stop_token stop_token,
    std::span<const RemoteItem> known_directories
) const {
    if (stop_token.stop_requested()) {
        throw RequestCancelledError{"Microsoft Graph delta request cancelled"};
    }
    const auto token = access_token(stop_token);
    const auto validation = options_.relaxed_download_validation
                                ? ContentValidation::relaxed
                                : ContentValidation::strict;

    std::string next_url;
    if (delta_link) {
        next_url = *delta_link;
        spdlog::info("Resuming Microsoft Graph delta query");
    } else {
        next_url = graph_drive_prefix(options_) + "/root/delta";
        spdlog::info("Starting initial Microsoft Graph delta query");
    }

    std::unordered_set<std::string> visited_urls;
    DeltaChanges changes;
    // A package parent can arrive on a later page than its children.
    std::unordered_map<std::string, Json> hash_metadata;
    DeltaResult result;
    std::size_t page_number = 1;
    std::size_t scanned_item_count = 0;
    while (!next_url.empty()) {
        if (stop_token.stop_requested()) {
            throw RequestCancelledError{
                "Microsoft Graph delta request cancelled"
            };
        }
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
                    "Authorization: Bearer " + token,
                },
                .body = {},
                .stop_token = stop_token,
            });
            },
            options_,
            options_.maximum_throttle_retries,
            sleep_,
            std::format("Microsoft Graph delta page {}", page_number),
            stop_token
        );
        if (!response) {
            if (stop_token.stop_requested() ||
                response.error().code == http::HttpErrorCode::cancelled) {
                throw RequestCancelledError{
                    "Microsoft Graph delta request cancelled"
                };
            }
            throw std::runtime_error(
                "Microsoft Graph delta request failed: " +
                response.error().message
            );
        }
        if (stop_token.stop_requested()) {
            throw RequestCancelledError{
                "Microsoft Graph delta request cancelled"
            };
        }

        const auto json = parse_graph_json(*response, "delta");
        if (delta_link && response->status_code == http::status::gone) {
            throw DeltaCursorInvalidError(
                "Microsoft Graph rejected the saved delta cursor: " +
                graph_error_message(json, response->status_code)
            );
        }
        require_successful_graph_response(
            json, response->status_code, {}, options_.drive_id
        );

        try {
            const auto& values =
                require_graph_page_values(json, "delta");
            const std::size_t page_item_count = values.size();
            for (const auto& value : values) {
                if (stop_token.stop_requested()) {
                    throw RequestCancelledError{
                        "Microsoft Graph delta request cancelled"
                    };
                }
                auto item = parse_delta_item(value, validation);
                hash_metadata.erase(item.id);
                if (!item.deleted && item.unsupported_format.empty()) {
                    if (const auto file = value.find("file");
                        file != value.end()) {
                        hash_metadata.emplace(
                            item.id,
                            Json{
                                {"id", item.id},
                                {"name", item.name},
                                {"file", *file}
                            }
                        );
                    }
                }
                changes.upsert(std::move(item));
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
                "Received Microsoft Graph delta page {} with {} changes; {} "
                "total",
                page_number,
                page_item_count,
                changes.size()
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

    result.changes =
        DeltaHierarchyResolver{
            std::move(changes).release(),
            known_directories,
            [&](const std::string& id) {
                return ancestor_by_id(id, stop_token);
            },
            stop_token
        }
            .resolve();
    for (auto& item : result.changes) {
        if (item.deleted || !item.unsupported_format.empty()) {
            continue;
        }
        if (!item.directory && item.last_modified.empty()) {
            throw std::runtime_error(
                "Microsoft Graph delta file is missing "
                "fileSystemInfo.lastModifiedDateTime"
            );
        }
        if (const auto metadata = hash_metadata.find(item.id);
            metadata != hash_metadata.end()) {
            item.content_hash = item_content_hash(metadata->second, validation);
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
