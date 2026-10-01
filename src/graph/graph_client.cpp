#include "onedrive/graph/graph_client.hpp"

#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/http/http_client.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <format>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
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

std::vector<RemoteItem> MicrosoftGraphClient::list_root() const {
    const auto refresh_token = token_store_->load_refresh_token();
    if (!refresh_token) {
        throw std::runtime_error(
            "no saved Microsoft authentication is available; run 'onedrive-cpp auth'"
        );
    }

    auto tokens = auth_client_->refresh_access_token(*refresh_token);
    if (!tokens) {
        throw std::runtime_error(
            "cannot refresh Microsoft access token: " + tokens.error().message
        );
    }
    if (tokens->refresh_token != *refresh_token) {
        token_store_->save_refresh_token(tokens->refresh_token);
    }

    std::string next_url =
        options_.drive_id == "me" ?
            options_.endpoint + "/me/drive/root/children" :
            options_.endpoint + "/drives/" + percent_encode(options_.drive_id) +
                "/root/children";
    const std::string allowed_url_prefix = options_.endpoint + "/";
    std::unordered_set<std::string> visited_urls;
    std::vector<RemoteItem> items;

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
            sleep_(
                server_delay.value_or(
                    fallback_retry_delay(options_, throttle_retries)
                )
            );
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
            for (const auto& value : values) {
                RemoteItem item{
                    .id = value.at("id").get<std::string>(),
                    .name = value.at("name").get<std::string>(),
                    .etag = value.at("eTag").get<std::string>(),
                    .directory = value.contains("folder"),
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
        } catch (const Json::exception& error) {
            throw std::runtime_error(
                "Microsoft Graph response is missing required drive item data: " +
                std::string{error.what()}
            );
        }
    }

    return items;
}

}  // namespace onedrive::graph
