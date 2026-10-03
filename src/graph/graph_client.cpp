#include "onedrive/graph/graph_client.hpp"

#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/http/http_client.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <format>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace onedrive::graph {
namespace {

using Json = nlohmann::json;

std::string percent_encode(std::string_view value) {
    constexpr std::string_view hex{"0123456789ABCDEF"};
    std::string encoded;
    encoded.reserve(value.size());
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        const bool unreserved =
            (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '-' ||
            character == '_' || character == '.' || character == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[character >> 4U]);
            encoded.push_back(hex[character & 0x0FU]);
        }
    }
    return encoded;
}

std::string graph_error_message(const Json& response, long status_code) {
    try {
        if (const auto error = response.find("error");
            error != response.end() && error->is_object()) {
            if (const auto message = error->find("message");
                message != error->end() && message->is_string()) {
                return message->get<std::string>();
            }
        }
    } catch (const Json::exception&) {
    }
    return std::format("Microsoft Graph request failed with HTTP {}", status_code);
}

std::string normalized_endpoint(std::string endpoint) {
    while (endpoint.ends_with('/')) {
        endpoint.pop_back();
    }
    return endpoint;
}

bool equal_case_insensitive(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto left_character =
            static_cast<unsigned char>(left[index]);
        const auto right_character =
            static_cast<unsigned char>(right[index]);
        if (std::tolower(left_character) != std::tolower(right_character)) {
            return false;
        }
    }
    return true;
}

std::optional<std::chrono::seconds> retry_after(
    const http::HttpResponse& response
) {
    for (const auto& header : response.headers) {
        if (!equal_case_insensitive(header.name, "Retry-After")) {
            continue;
        }

        std::int64_t seconds{};
        const auto* begin = header.value.data();
        const auto* end = begin + header.value.size();
        const auto [position, error] = std::from_chars(begin, end, seconds);
        if (error == std::errc{} && position == end && seconds >= 0) {
            return std::chrono::seconds{seconds};
        }
    }
    return std::nullopt;
}

std::optional<std::string> header_value(
    const http::HttpResponse& response,
    std::string_view name
) {
    for (const auto& header : response.headers) {
        if (equal_case_insensitive(header.name, name)) {
            return header.value;
        }
    }
    return std::nullopt;
}

std::chrono::seconds fallback_retry_delay(
    const GraphOptions& options,
    std::size_t retry_number
) {
    auto delay = options.initial_throttle_delay;
    for (std::size_t index = 0;
         index < retry_number && delay < options.maximum_throttle_delay;
         ++index) {
        delay = std::min(delay * 2, options.maximum_throttle_delay);
    }
    return delay;
}

std::string item_remote_path(const Json& value, const std::string& name) {
    const auto parent = value.find("parentReference");
    if (parent == value.end() || !parent->is_object()) {
        return name;
    }
    const auto path = parent->find("path");
    if (path == parent->end() || !path->is_string()) {
        return name;
    }

    const std::string parent_path = path->get<std::string>();
    const auto root_marker = parent_path.find("root:");
    if (root_marker == std::string::npos) {
        return name;
    }
    std::string relative_parent = parent_path.substr(root_marker + 5);
    while (relative_parent.starts_with('/')) {
        relative_parent.erase(relative_parent.begin());
    }
    return relative_parent.empty() ? name : relative_parent + '/' + name;
}

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
        options_.maximum_throttle_delay < options_.initial_throttle_delay) {
        throw std::invalid_argument(
            "Microsoft Graph client requires valid throttle retry delays"
        );
    }
    auth_client_ =
        std::make_unique<auth::DeviceAuthClient>(*transport_, std::move(auth_options));
}

MicrosoftGraphClient::~MicrosoftGraphClient() = default;

account::DriveIdentity fetch_drive_identity(
    const http::HttpTransport& transport,
    std::string_view access_token,
    GraphOptions options
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
            .maximum_response_size = 1024U * 1024U,
        });
        if (!response) {
            throw std::runtime_error(
                "Microsoft Graph " + std::string{name} +
                " request failed: " + response.error().message
            );
        }
        Json json;
        try {
            json = Json::parse(response->body);
        } catch (const Json::exception& error) {
            throw std::runtime_error(
                "Microsoft Graph returned invalid " + std::string{name} +
                " JSON: " + error.what()
            );
        }
        if (response->status_code < 200 || response->status_code >= 300) {
            throw std::runtime_error(
                "Microsoft Graph " + std::string{name} + " query failed: " +
                graph_error_message(json, response->status_code)
            );
        }
        return json;
    };

    const auto user = request_json(
        options.endpoint + "/me?$select=id,displayName",
        "user identity"
    );
    const auto drive_url =
        options.drive_id == "me" ?
            options.endpoint + "/me/drive?$select=id,name" :
            options.endpoint + "/drives/" + percent_encode(options.drive_id) +
                "?$select=id,name";
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
        .maximum_response_size = 8U * 1024U * 1024U,
    });
    if (!photo) {
        throw std::runtime_error(
            "Microsoft Graph profile photo request failed: " +
            photo.error().message
        );
    }
    if (photo->status_code == 404) {
        spdlog::debug("Microsoft account has no profile photo");
    } else if (photo->status_code >= 200 && photo->status_code < 300) {
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
        options_.drive_id == "me" ?
            options_.endpoint + "/me/drive/root/children" :
            options_.endpoint + "/drives/" + percent_encode(options_.drive_id) +
                "/root/children";
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

        http::HttpResult response;
        std::size_t throttle_retries = 0;
        while (true) {
            spdlog::debug(
                "Requesting Microsoft Graph root page {} (attempt {})",
                page_number,
                throttle_retries + 1
            );
            response = transport_->perform(http::HttpRequest{
                .method = http::HttpMethod::get,
                .url = next_url,
                .headers = {
                    "Accept: application/json",
                    "Authorization: Bearer " + tokens->access_token,
                },
                .body = {},
            });
            if (!response) {
                throw std::runtime_error(
                    "Microsoft Graph request failed: " + response.error().message
                );
            }
            if (response->status_code != 429) {
                break;
            }
            if (throttle_retries >= options_.maximum_throttle_retries) {
                throw std::runtime_error(
                    std::format(
                        "Microsoft Graph throttling persisted after {} retries",
                        throttle_retries
                    )
                );
            }

            const auto server_delay = retry_after(*response);
            if (server_delay &&
                *server_delay > options_.maximum_throttle_delay) {
                throw std::runtime_error(
                    "Microsoft Graph Retry-After exceeds the configured maximum "
                    "throttle delay"
                );
            }
            const auto delay = server_delay.value_or(
                fallback_retry_delay(options_, throttle_retries)
            );
            spdlog::warn(
                "Microsoft Graph throttled root page {}; retrying in {} seconds "
                "({}/{})",
                page_number,
                delay.count(),
                throttle_retries + 1,
                options_.maximum_throttle_retries
            );
            sleep_(delay);
            ++throttle_retries;
        }

        Json json;
        try {
            json = Json::parse(response->body);
        } catch (const Json::exception& error) {
            throw std::runtime_error(
                "Microsoft Graph returned invalid JSON: " +
                std::string{error.what()}
            );
        }
        if (response->status_code < 200 || response->status_code >= 300) {
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
                    .parent_id = {},
                    .remote_path = {},
                    .last_modified = {},
                    .size = 0,
                    .directory = value.contains("folder"),
                    .deleted = false,
                };
                if (item.id.empty() || item.name.empty() || item.etag.empty()) {
                    throw std::runtime_error(
                        "Microsoft Graph returned a drive item with empty metadata"
                    );
                }
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

DeltaResult MicrosoftGraphClient::list_delta(
    const std::optional<std::string>& delta_link,
    const DeltaProgress& progress
) const {
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

    const std::string allowed_url_prefix = options_.endpoint + "/";
    std::string next_url;
    if (delta_link) {
        next_url = *delta_link;
        spdlog::info("Resuming Microsoft Graph delta query");
    } else {
        next_url =
            options_.drive_id == "me" ?
                options_.endpoint + "/me/drive/root/delta" :
                options_.endpoint + "/drives/" + percent_encode(options_.drive_id) +
                    "/root/delta";
        spdlog::info("Starting initial Microsoft Graph delta query");
    }

    std::unordered_set<std::string> visited_urls;
    std::unordered_map<std::string, std::size_t> change_indexes;
    DeltaResult result;
    std::size_t page_number = 1;
    std::size_t scanned_item_count = 0;
    while (!next_url.empty()) {
        if (!next_url.starts_with(allowed_url_prefix)) {
            throw std::runtime_error(
                "Microsoft Graph returned a delta URL outside its endpoint"
            );
        }
        if (!visited_urls.insert(next_url).second) {
            throw std::runtime_error(
                "Microsoft Graph returned a repeated delta URL"
            );
        }

        http::HttpResult response;
        std::size_t throttle_retries = 0;
        while (true) {
            spdlog::debug(
                "Requesting Microsoft Graph delta page {} (attempt {})",
                page_number,
                throttle_retries + 1
            );
            response = transport_->perform(http::HttpRequest{
                .method = http::HttpMethod::get,
                .url = next_url,
                .headers = {
                    "Accept: application/json",
                    "Authorization: Bearer " + tokens->access_token,
                },
                .body = {},
            });
            if (!response) {
                throw std::runtime_error(
                    "Microsoft Graph delta request failed: " +
                    response.error().message
                );
            }
            if (response->status_code != 429) {
                break;
            }
            if (throttle_retries >= options_.maximum_throttle_retries) {
                throw std::runtime_error(
                    std::format(
                        "Microsoft Graph throttling persisted after {} retries",
                        throttle_retries
                    )
                );
            }

            const auto server_delay = retry_after(*response);
            if (server_delay &&
                *server_delay > options_.maximum_throttle_delay) {
                throw std::runtime_error(
                    "Microsoft Graph Retry-After exceeds the configured maximum "
                    "throttle delay"
                );
            }
            const auto delay = server_delay.value_or(
                fallback_retry_delay(options_, throttle_retries)
            );
            spdlog::warn(
                "Microsoft Graph throttled delta page {}; retrying in {} seconds "
                "({}/{})",
                page_number,
                delay.count(),
                throttle_retries + 1,
                options_.maximum_throttle_retries
            );
            sleep_(delay);
            ++throttle_retries;
        }

        Json json;
        try {
            json = Json::parse(response->body);
        } catch (const Json::exception& error) {
            throw std::runtime_error(
                "Microsoft Graph returned invalid delta JSON: " +
                std::string{error.what()}
            );
        }
        if (response->status_code < 200 || response->status_code >= 300) {
            if (delta_link && response->status_code == 410) {
                throw DeltaCursorInvalidError(
                    "Microsoft Graph rejected the saved delta cursor: " +
                    graph_error_message(json, response->status_code)
                );
            }
            throw std::runtime_error(
                graph_error_message(json, response->status_code)
            );
        }

        try {
            const auto& values = json.at("value");
            if (!values.is_array()) {
                throw std::runtime_error(
                    "Microsoft Graph delta response field 'value' is not an array"
                );
            }
            const std::size_t page_item_count = values.size();
            for (const auto& value : values) {
                RemoteItem item{};
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
                    if (const auto modified = value.find("lastModifiedDateTime");
                        modified != value.end() && modified->is_string()) {
                        item.last_modified = modified->get<std::string>();
                    }
                    if (const auto size = value.find("size");
                        size != value.end() && size->is_number_integer()) {
                        item.size = size->get<std::int64_t>();
                    }
                    if (item.name.empty() ||
                        (!item.root && item.remote_path.empty()) || item.size < 0) {
                        throw std::runtime_error(
                            "Microsoft Graph returned a delta item with invalid metadata"
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

            next_url.clear();
            if (const auto next = json.find("@odata.nextLink");
                next != json.end()) {
                if (!next->is_string()) {
                    throw std::runtime_error(
                        "Microsoft Graph returned an invalid delta pagination URL"
                    );
                }
                next_url = next->get<std::string>();
            } else {
                const auto final_link = json.find("@odata.deltaLink");
                if (final_link == json.end() || !final_link->is_string()) {
                    throw std::runtime_error(
                        "Microsoft Graph delta response did not contain a final "
                        "delta link"
                    );
                }
                result.delta_link = final_link->get<std::string>();
                if (!result.delta_link.starts_with(allowed_url_prefix)) {
                    throw std::runtime_error(
                        "Microsoft Graph returned a delta URL outside its endpoint"
                    );
                }
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
            const bool completed = next_url.empty();
            spdlog::info(
                "Microsoft Graph delta progress: {} pages, {} items scanned ({})",
                page_number,
                scanned_item_count,
                completed ? "complete" : "continuing"
            );
            if (progress) {
                progress(page_number, scanned_item_count, completed);
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

void MicrosoftGraphClient::download_file(
    const std::string& remote_id,
    std::uint64_t expected_size,
    const std::filesystem::path& destination,
    const DownloadProgress& progress
) const {
    if (remote_id.empty()) {
        throw std::invalid_argument("cannot download a drive item without an ID");
    }
    const std::string content_url =
        options_.drive_id == "me" ?
            options_.endpoint + "/me/drive/items/" + percent_encode(remote_id) +
                "/content" :
            options_.endpoint + "/drives/" + percent_encode(options_.drive_id) +
                "/items/" + percent_encode(remote_id) + "/content";

    auto redirect = transport_->perform(http::HttpRequest{
        .method = http::HttpMethod::get,
        .url = content_url,
        .headers = {
            "Accept: application/octet-stream",
            "Authorization: Bearer " + access_token(),
        },
        .body = {},
        .connect_timeout = std::chrono::seconds{30},
        .operation_timeout = std::chrono::seconds{60},
        .maximum_response_size = 64U * 1024U,
    });
    if (!redirect) {
        throw std::runtime_error(
            "Microsoft Graph download request failed: " + redirect.error().message
        );
    }
    if (redirect->status_code < 300 || redirect->status_code >= 400) {
        throw std::runtime_error(
            std::format(
                "Microsoft Graph download did not return a redirect (HTTP {})",
                redirect->status_code
            )
        );
    }
    const auto location = header_value(*redirect, "Location");
    if (!location || !location->starts_with("https://")) {
        throw std::runtime_error(
            "Microsoft Graph download returned an invalid HTTPS redirect"
        );
    }

    spdlog::trace(
        "Received HTTPS download redirect for Microsoft Graph drive item '{}'",
        remote_id
    );
    const auto download = [&](std::vector<std::string> headers,
                              std::uint64_t offset,
                              const DownloadProgress& chunk_progress) {
        return transport_->download(
            http::HttpRequest{
                .method = http::HttpMethod::get,
                .url = *location,
                .headers = std::move(headers),
                .body = {},
                .connect_timeout = std::chrono::seconds{30},
                .operation_timeout = std::chrono::hours{1},
                .maximum_response_size = 0,
                .download_offset = offset,
            },
            destination,
            chunk_progress
        );
    };

    spdlog::debug("Downloading Microsoft Graph drive item '{}'", remote_id);
    if (expected_size <= options_.download_chunk_threshold_bytes) {
        auto response = download(
            {"Accept: application/octet-stream"},
            0,
            progress
        );
        if (!response) {
            throw std::runtime_error(
                "Microsoft Graph file download failed: " +
                response.error().message
            );
        }
        if (response->status_code < 200 || response->status_code >= 300) {
            throw std::runtime_error(
                std::format(
                    "Microsoft Graph file download failed with HTTP {}",
                    response->status_code
                )
            );
        }
        spdlog::debug("Downloaded Microsoft Graph drive item '{}'", remote_id);
        return;
    }

    const auto chunk_size = options_.download_chunk_threshold_bytes;
    spdlog::debug(
        "Downloading Microsoft Graph drive item '{}' in {}-byte chunks",
        remote_id,
        chunk_size
    );
    for (std::uint64_t offset = 0; offset < expected_size;) {
        const auto bytes = std::min(chunk_size, expected_size - offset);
        const auto end = offset + bytes - 1;
        auto response = download(
            {
                "Accept: application/octet-stream",
                std::format("Range: bytes={}-{}", offset, end),
            },
            offset,
            progress ?
                DownloadProgress{
                    [&, offset](std::uint64_t downloaded, std::uint64_t) {
                        progress(
                            std::min(offset + downloaded, expected_size),
                            expected_size
                        );
                    }
                } :
                DownloadProgress{}
        );
        if (!response) {
            throw std::runtime_error(
                "Microsoft Graph chunk download failed: " +
                response.error().message
            );
        }
        if (response->status_code != 206) {
            throw std::runtime_error(
                std::format(
                    "Microsoft Graph chunk download expected HTTP 206 but "
                    "received HTTP {}",
                    response->status_code
                )
            );
        }
        offset += bytes;
    }
    spdlog::debug("Downloaded Microsoft Graph drive item '{}'", remote_id);
}

}  // namespace onedrive::graph
