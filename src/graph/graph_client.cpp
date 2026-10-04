#include "onedrive/graph/graph_client.hpp"

#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/http/http_client.hpp"
#include "onedrive/remote_time.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <format>
#include <optional>
#include <mutex>
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
    if (const auto error = response.find("error");
        error != response.end() && error->is_object()) {
        if (const auto message = error->find("message");
            message != error->end() && message->is_string()) {
            return message->get<std::string>();
        }
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

struct ContentRange {
    std::uint64_t first;
    std::uint64_t last;
    std::uint64_t total;
};

[[noreturn]] void invalid_content_range(std::string_view header) {
    throw std::runtime_error(
        "Microsoft Graph chunk download returned an invalid "
        "Content-Range header: " + std::string{header}
    );
}

std::uint64_t parse_content_range_number(
    std::string_view value,
    std::string_view header
) {
    std::uint64_t number{};
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto [position, error] = std::from_chars(begin, end, number);
    if (value.empty() || error != std::errc{} || position != end) {
        invalid_content_range(header);
    }
    return number;
}

ContentRange parse_content_range(std::string_view header) {
    constexpr std::string_view prefix{"bytes "};
    if (!header.starts_with(prefix)) {
        invalid_content_range(header);
    }
    header.remove_prefix(prefix.size());
    const auto dash = header.find('-');
    const auto slash = header.find('/');
    if (dash == std::string_view::npos ||
        slash == std::string_view::npos ||
        dash == 0 || slash <= dash + 1 || slash + 1 >= header.size()) {
        invalid_content_range(header);
    }
    const ContentRange range{
        .first = parse_content_range_number(header.substr(0, dash), header),
        .last = parse_content_range_number(
            header.substr(dash + 1, slash - dash - 1),
            header
        ),
        .total = parse_content_range_number(header.substr(slash + 1), header),
    };
    if (range.first > range.last || range.last >= range.total) {
        invalid_content_range(header);
    }
    return range;
}

void validate_chunk_response_metadata(
    const http::HttpResponse& response,
    std::uint64_t expected_first,
    std::uint64_t expected_last,
    std::uint64_t expected_total
) {
    if (response.status_code != 206) {
        throw std::runtime_error(
            std::format(
                "Microsoft Graph chunk download expected HTTP 206 but "
                "received HTTP {}",
                response.status_code
            )
        );
    }
    std::optional<std::string_view> header;
    for (const auto& candidate : response.headers) {
        if (!equal_case_insensitive(candidate.name, "Content-Range")) {
            continue;
        }
        if (header.has_value()) {
            throw std::runtime_error(
                "Microsoft Graph chunk download returned multiple "
                "Content-Range headers"
            );
        }
        header = candidate.value;
    }
    if (!header.has_value()) {
        throw std::runtime_error(
            "Microsoft Graph chunk download did not return Content-Range"
        );
    }
    const auto range = parse_content_range(*header);
    if (range.first != expected_first ||
        range.last != expected_last ||
        range.total != expected_total) {
        throw std::runtime_error(
            std::format(
                "Microsoft Graph chunk download returned Content-Range "
                "'{}'; expected 'bytes {}-{}/{}'",
                *header,
                expected_first,
                expected_last,
                expected_total
            )
        );
    }
}

void validate_chunk_response(
    const http::HttpResponse& response,
    std::uint64_t expected_first,
    std::uint64_t expected_last,
    std::uint64_t expected_total
) {
    validate_chunk_response_metadata(
        response,
        expected_first,
        expected_last,
        expected_total
    );
    const auto expected_bytes = expected_last - expected_first + 1;
    if (response.received_size != expected_bytes) {
        throw std::runtime_error(
            std::format(
                "Microsoft Graph chunk download wrote {} bytes; expected {}",
                response.received_size,
                expected_bytes
            )
        );
    }
}

void roll_back_chunk(
    const std::filesystem::path& destination,
    std::uint64_t offset
) {
    std::error_code error;
    if (offset == 0) {
        std::filesystem::remove(destination, error);
    } else {
        std::filesystem::resize_file(destination, offset, error);
    }
    if (error) {
        throw std::runtime_error(
            "cannot roll back rejected Microsoft Graph chunk at byte " +
            std::to_string(offset) + ": " + error.message()
        );
    }
}

[[noreturn]] void throw_download_error(
    std::string_view description,
    const http::HttpError& error
) {
    if (error.code == http::HttpErrorCode::cancelled) {
        throw DownloadCancelledError(
            std::string{description} + " was cancelled"
        );
    }
    throw std::runtime_error(
        std::string{description} + " failed: " + error.message
    );
}

bool successful_download_status(long status_code) {
    return status_code >= 200 && status_code < 300;
}

bool accept_successful_download_response(
    long status_code,
    std::span<const http::HttpHeader>
) {
    return successful_download_status(status_code);
}

void require_successful_download(
    const http::HttpResult& response,
    std::string_view description
) {
    if (!response) {
        throw_download_error(description, response.error());
    }
    if (!successful_download_status(response->status_code)) {
        throw std::runtime_error(
            std::format(
                "{} failed with HTTP {}",
                description,
                response->status_code
            )
        );
    }
}

std::chrono::seconds fallback_retry_delay(
    const GraphOptions& options,
    std::size_t retry_number
) {
    auto delay = options.initial_throttle_delay;
    for (std::size_t index = 0;
         index < retry_number && delay < options.maximum_throttle_delay;
         ++index) {
        const auto remaining = options.maximum_throttle_delay - delay;
        delay = delay > remaining ?
                    options.maximum_throttle_delay :
                    delay + delay;
    }
    return delay;
}

bool retryable_status(long status_code) {
    return status_code == 408 || status_code == 429 || status_code == 502 ||
           status_code == 503 || status_code == 504;
}

bool stale_download_url_status(long status_code) {
    return status_code == 401 || status_code == 403;
}

http::HttpResult cancelled_http_result() {
    return std::unexpected(http::HttpError{
        .code = http::HttpErrorCode::cancelled,
        .message = "HTTP request was cancelled",
    });
}

bool wait_for_retry(
    std::chrono::seconds delay,
    const MicrosoftGraphClient::SleepFunction& sleep,
    const std::stop_token& stop_token
) {
    if (!stop_token.stop_possible()) {
        sleep(delay);
        return false;
    }
    std::mutex mutex;
    std::condition_variable_any condition;
    std::unique_lock lock{mutex};
    condition.wait_for(lock, stop_token, delay, [] { return false; });
    return stop_token.stop_requested();
}

template <typename Operation>
http::HttpResult perform_with_retries(
    Operation operation,
    const GraphOptions& options,
    std::size_t maximum_retries,
    const MicrosoftGraphClient::SleepFunction& sleep,
    std::string_view description,
    const std::stop_token& stop_token = {},
    bool retry_transport_errors = false
) {
    std::size_t retries = 0;
    while (true) {
        if (stop_token.stop_requested()) {
            return cancelled_http_result();
        }
        auto response = operation();
        const bool transport_error = !response;
        if (transport_error &&
            (response.error().code == http::HttpErrorCode::cancelled ||
             !retry_transport_errors)) {
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

std::optional<std::string> file_system_last_modified(
    const Json& value,
    std::string_view description
) {
    const auto file_system_info = value.find("fileSystemInfo");
    if (file_system_info == value.end()) {
        return std::nullopt;
    }
    if (!file_system_info->is_object()) {
        throw std::runtime_error(
            "Microsoft Graph returned an invalid " +
            std::string{description} + " fileSystemInfo facet"
        );
    }
    const auto modified = file_system_info->find("lastModifiedDateTime");
    if (modified == file_system_info->end()) {
        return std::nullopt;
    }
    if (!modified->is_string() ||
        modified->get_ref<const std::string&>().empty()) {
        throw std::runtime_error(
            "Microsoft Graph returned an invalid " +
            std::string{description} +
            " fileSystemInfo.lastModifiedDateTime"
        );
    }
    auto timestamp = modified->get<std::string>();
    static_cast<void>(parse_remote_modified_time(timestamp));
    return timestamp;
}

const Json* remote_item_facet(const Json& value) {
    const auto remote_item = value.find("remoteItem");
    if (remote_item == value.end()) {
        return nullptr;
    }
    if (!remote_item->is_object()) {
        throw std::runtime_error(
            "Microsoft Graph returned an invalid remoteItem facet"
        );
    }
    return &*remote_item;
}

std::optional<std::string> authoritative_last_modified(const Json& value) {
    if (const auto* remote_item = remote_item_facet(value);
        remote_item != nullptr) {
        if (auto modified =
                file_system_last_modified(*remote_item, "remoteItem");
            modified.has_value()) {
            return modified;
        }
    }
    return file_system_last_modified(value, "drive item");
}

bool has_malware_facet(const Json& value, std::string_view description) {
    const auto malware = value.find("malware");
    if (malware == value.end()) {
        return false;
    }
    if (!malware->is_object()) {
        throw std::runtime_error(
            "Microsoft Graph returned an invalid " +
            std::string{description} + " malware facet"
        );
    }
    return true;
}

bool item_is_malware(const Json& value) {
    const auto* remote_item = remote_item_facet(value);
    return has_malware_facet(value, "drive item") ||
           (remote_item != nullptr &&
            has_malware_facet(*remote_item, "remoteItem"));
}

std::optional<FileHash> item_content_hash(const Json& value) {
    const auto file = value.find("file");
    if (file == value.end()) {
        return std::nullopt;
    }
    if (!file->is_object()) {
        throw std::runtime_error(
            "Microsoft Graph returned an invalid file facet"
        );
    }
    const auto hashes = file->find("hashes");
    if (hashes == file->end()) {
        return std::nullopt;
    }
    if (!hashes->is_object()) {
        throw std::runtime_error(
            "Microsoft Graph returned an invalid file hash facet"
        );
    }
    const auto read_hash = [&](std::string_view name)
        -> std::optional<std::string> {
        const auto hash = hashes->find(std::string{name});
        if (hash == hashes->end()) {
            return std::nullopt;
        }
        if (!hash->is_string() || hash->get_ref<const std::string&>().empty()) {
            throw std::runtime_error(
                "Microsoft Graph returned an invalid " +
                std::string{name}
            );
        }
        return hash->get<std::string>();
    };
    if (auto sha256 = read_hash("sha256Hash"); sha256.has_value()) {
        const bool valid =
            sha256.value().size() == 64 &&
            std::ranges::all_of(
                sha256.value(),
                [](unsigned char character) {
                    return std::isxdigit(character) != 0;
                }
            );
        if (!valid) {
            throw std::runtime_error(
                "Microsoft Graph returned an invalid sha256Hash"
            );
        }
        return FileHash{
            .algorithm = onedrive::FileHashAlgorithm::sha256,
            .value = std::move(sha256.value()),
        };
    }
    if (auto quick_xor = read_hash("quickXorHash");
        quick_xor.has_value()) {
        const bool valid =
            quick_xor.value().size() == 28 &&
            quick_xor.value().back() == '=' &&
            std::ranges::all_of(
                quick_xor.value().begin(),
                quick_xor.value().end() - 1,
                [](unsigned char character) {
                    return std::isalnum(character) != 0 ||
                           character == '+' || character == '/';
                }
            );
        if (!valid) {
            throw std::runtime_error(
                "Microsoft Graph returned an invalid quickXorHash"
            );
        }
        return FileHash{
            .algorithm = onedrive::FileHashAlgorithm::quick_xor,
            .value = std::move(quick_xor.value()),
        };
    }
    return std::nullopt;
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
    const auto& download_transport = options_.download_transport;
    if (options_.download_chunk_threshold_bytes == 0 ||
        options_.download_checkpoint_interval_bytes == 0 ||
        download_transport.connect_timeout <=
            std::chrono::seconds::zero() ||
        download_transport.operation_timeout <=
            std::chrono::seconds::zero() ||
        download_transport.low_speed_timeout < std::chrono::seconds::zero() ||
        download_transport.low_speed_limit_bytes_per_second == 0) {
        throw std::invalid_argument(
            "Microsoft Graph client requires valid download transport options"
        );
    }
    auth_client_ =
        std::make_unique<auth::DeviceAuthClient>(
            transport_.get(),
            std::move(auth_options)
        );
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
            .maximum_response_size = std::size_t{1024} * 1024U,
            .stop_token = {},
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
        .maximum_response_size = std::size_t{8} * 1024U * 1024U,
        .stop_token = {},
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
            spdlog::trace(
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
    std::uint64_t initial_offset,
    std::stop_token stop_token,
    const DownloadProgress& progress,
    const DownloadCheckpoint& checkpoint,
    const DownloadData& data
) const {
    const auto& download_transport = options_.download_transport;
    if (remote_id.empty()) {
        throw std::invalid_argument("cannot download a drive item without an ID");
    }
    if (initial_offset > expected_size) {
        throw std::invalid_argument(
            "download resume offset exceeds the expected file size"
        );
    }
    if (stop_token.stop_requested()) {
        throw DownloadCancelledError("Microsoft Graph download was cancelled");
    }
    if (options_.relaxed_download_validation && initial_offset != 0) {
        throw std::invalid_argument(
            "relaxed Graph downloads cannot resume from a partial file"
        );
    }
    if (!options_.relaxed_download_validation &&
        initial_offset != 0 &&
        initial_offset == expected_size) {
        if (progress) {
            progress(expected_size, expected_size);
        }
        if (checkpoint) {
            checkpoint(expected_size);
        }
        return;
    }
    const std::string content_url =
        options_.drive_id == "me" ?
            options_.endpoint + "/me/drive/items/" + percent_encode(remote_id) +
                "/content" :
            options_.endpoint + "/drives/" + percent_encode(options_.drive_id) +
                "/items/" + percent_encode(remote_id) + "/content";

    const auto request_download_url = [&]() {
        const auto redirect = perform_with_retries(
            [&] {
                return transport_->perform(http::HttpRequest{
                    .method = http::HttpMethod::get,
                    .url = content_url,
                    .headers = {
                        "Accept: application/octet-stream",
                        "Authorization: Bearer " + access_token(),
                    },
                    .body = {},
                    .connect_timeout = download_transport.connect_timeout,
                    .operation_timeout = std::chrono::seconds{60},
                    .maximum_response_size = std::size_t{64} * 1024U,
                    .stop_token = stop_token,
                });
            },
            options_,
            options_.maximum_throttle_retries,
            sleep_,
            "Microsoft Graph download redirect",
            stop_token
        );
        if (!redirect) {
            throw_download_error(
                "Microsoft Graph download request",
                redirect.error()
            );
        }
        if (redirect->status_code < 300 || redirect->status_code >= 400) {
            throw std::runtime_error(
                std::format(
                    "Microsoft Graph download did not return a redirect "
                    "(HTTP {})",
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
            "Received HTTPS download redirect for Microsoft Graph drive item "
            "'{}'",
            remote_id
        );
        return *location;
    };
    auto location = request_download_url();
    const auto low_speed_limit =
        download_transport.low_speed_limit_bytes_per_second;
    const auto maximum_receive_speed =
        download_transport.maximum_receive_speed_bytes_per_second;
    using DownloadRequest = std::pair<
        std::vector<std::string>,
        std::uint64_t
    >;
    const auto download = [&](const std::function<DownloadRequest()>& request,
                              const DownloadProgress& chunk_progress,
                              std::string_view description,
                              const DownloadCheckpoint& chunk_checkpoint = {},
                              const http::DownloadResponseGate& response_gate =
                                  {}) {
        const auto perform_download = [&] {
            return perform_with_retries(
                [&] {
                    auto [headers, offset] = request();
                    return transport_->download(
                        http::HttpRequest{
                            .method = http::HttpMethod::get,
                            .url = location,
                            .headers = headers,
                            .body = {},
                            .connect_timeout =
                                download_transport.connect_timeout,
                            .operation_timeout =
                                download_transport.operation_timeout,
                            .low_speed_timeout =
                                download_transport.low_speed_timeout,
                            .low_speed_limit_bytes_per_second =
                                low_speed_limit,
                            .maximum_receive_speed_bytes_per_second =
                                maximum_receive_speed,
                            .http_version =
                                download_transport.http_version,
                            .ip_version =
                                download_transport.ip_version,
                            .maximum_response_size = 0,
                            .download_offset = offset,
                            .download_checkpoint_interval_bytes =
                                options_.download_checkpoint_interval_bytes,
                            .private_download_permissions =
                                options_.private_download_permissions,
                            .stop_token = stop_token,
                        },
                        destination,
                        chunk_progress,
                        data,
                        chunk_checkpoint,
                        response_gate
                    );
                },
                options_,
                options_.download_maximum_retries,
                sleep_,
                description,
                stop_token,
                true
            );
        };
        auto response = perform_download();
        if (response &&
            stale_download_url_status(response->status_code)) {
            spdlog::warn(
                "{} returned HTTP {}; refreshing the Microsoft Graph download "
                "redirect",
                description,
                response->status_code
            );
            location = request_download_url();
            response = perform_download();
        }
        return response;
    };

    spdlog::debug("Downloading Microsoft Graph drive item '{}'", remote_id);
    if (options_.relaxed_download_validation) {
        auto response = download(
            [] {
                return DownloadRequest{
                    {"Accept: application/octet-stream"},
                    0,
                };
            },
            progress,
            "Microsoft Graph relaxed file download",
            {},
            accept_successful_download_response
        );
        require_successful_download(
            response,
            "Microsoft Graph relaxed file download"
        );
        spdlog::warn(
            "Downloaded Microsoft Graph drive item '{}' without relying on "
            "remote size or hash metadata",
            remote_id
        );
        return;
    }
    if (initial_offset == 0 &&
        expected_size <= options_.download_chunk_threshold_bytes) {
        auto response = download(
            [] {
                return DownloadRequest{
                    {"Accept: application/octet-stream"},
                    0,
                };
            },
            progress,
            "Microsoft Graph file download",
            checkpoint,
            accept_successful_download_response
        );
        require_successful_download(
            response,
            "Microsoft Graph file download"
        );
        spdlog::debug("Downloaded Microsoft Graph drive item '{}'", remote_id);
        return;
    }

    const auto chunk_size = options_.download_chunk_threshold_bytes;
    spdlog::debug(
        "Downloading Microsoft Graph drive item '{}' in {}-byte chunks",
        remote_id,
        chunk_size
    );
    for (std::uint64_t offset = initial_offset; offset < expected_size;) {
        const auto bytes = std::min(chunk_size, expected_size - offset);
        const auto end = offset + bytes - 1;
        std::uint64_t durable_offset = offset;
        std::uint64_t attempt_offset = offset;
        auto response = download(
            [&] {
                attempt_offset = durable_offset;
                return DownloadRequest{
                    {
                        "Accept: application/octet-stream",
                        std::format(
                            "Range: bytes={}-{}",
                            attempt_offset,
                            end
                        ),
                    },
                    attempt_offset,
                };
            },
            progress ?
                DownloadProgress{
                    [&](std::uint64_t downloaded, std::uint64_t) {
                        progress(
                            std::min(
                                attempt_offset + downloaded,
                                expected_size
                            ),
                            expected_size
                        );
                    }
                } :
                DownloadProgress{},
            std::format(
                "Microsoft Graph chunk download (bytes {}-{})",
                offset,
                end
            ),
            [&](std::uint64_t completed) {
                if (completed < durable_offset || completed > end + 1) {
                    throw std::logic_error(
                        "download transport checkpoint is outside the "
                        "requested byte range"
                    );
                }
                durable_offset = completed;
                if (checkpoint) {
                    checkpoint(completed);
                }
            },
            [&](
                long status_code,
                std::span<const http::HttpHeader> headers
            ) {
                try {
                    validate_chunk_response_metadata(
                        http::HttpResponse{
                            .status_code = status_code,
                            .headers = {
                                headers.begin(),
                                headers.end(),
                            },
                            .body = {},
                            .received_size = 0,
                        },
                        attempt_offset,
                        end,
                        expected_size
                    );
                    return true;
                } catch (...) {
                    return false;
                }
            }
        );
        if (!response) {
            throw_download_error(
                "Microsoft Graph chunk download",
                response.error()
            );
        }
        try {
            validate_chunk_response(
                *response,
                attempt_offset,
                end,
                expected_size
            );
        } catch (...) {
            roll_back_chunk(destination, offset);
            throw;
        }
        offset = end + 1;
    }
    spdlog::debug("Downloaded Microsoft Graph drive item '{}'", remote_id);
}

}  // namespace onedrive::graph
