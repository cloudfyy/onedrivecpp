#include "graph/support.hpp"

#include "onedrive/util/remote_time.hpp"
#include "util/ascii.hpp"
#include "util/uri.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <stdexcept>
#include <utility>

namespace onedrive::graph::client_detail {

using onedrive::util::percent_encode_uri_component;

std::string percent_encode_remote_path(std::string_view path) {
    if (path.empty() || path.starts_with('/') || path.ends_with('/')) {
        throw std::invalid_argument(
            "remote file path must be a non-empty relative path"
        );
    }

    std::string encoded;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto separator = path.find('/', start);
        const auto length =
            separator == std::string_view::npos ?
                path.size() - start :
                separator - start;
        const auto segment = path.substr(start, length);
        if (segment.empty() || segment == "." || segment == ".." ||
            segment.find('\\') != std::string_view::npos) {
            throw std::invalid_argument(
                "remote file path contains an unsafe segment"
            );
        }
        if (!encoded.empty()) {
            encoded.push_back('/');
        }
        encoded += percent_encode_uri_component(segment);
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
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

std::string graph_error_code(const Json& response) {
    if (const auto error = response.find("error");
        error != response.end() && error->is_object()) {
        if (const auto code = error->find("code");
            code != error->end() && code->is_string()) {
            return code->get<std::string>();
        }
    }
    return {};
}

bool upload_quota_error(const Json& response, long status_code) {
    if (status_code == 507) {
        return true;
    }
    const auto code = graph_error_code(response);
    return util::ascii_iequals(code, "quotaLimitReached") ||
           util::ascii_iequals(code, "storageLimitExceeded") ||
           util::ascii_iequals(code, "insufficientStorage");
}

[[noreturn]] void throw_upload_response_error(
    const Json& response,
    long status_code
) {
    const auto message = graph_error_message(response, status_code);
    if (upload_quota_error(response, status_code)) {
        throw UploadResourceError{"remote_quota", message};
    }
    throw std::runtime_error(message);
}

std::string normalized_endpoint(std::string endpoint) {
    while (endpoint.ends_with('/')) {
        endpoint.pop_back();
    }
    return endpoint;
}

std::string graph_drive_prefix(const GraphOptions& options) {
    return options.drive_id == "me" ?
        options.endpoint + "/me/drive" :
        options.endpoint + "/drives/" +
            percent_encode_uri_component(options.drive_id);
}

bool successful_status(long status_code) noexcept {
    return status_code >= 200 && status_code < 300;
}

void require_successful_graph_response(
    const Json& response,
    long status_code,
    std::string_view description
) {
    if (successful_status(status_code)) {
        return;
    }
    const auto message = graph_error_message(response, status_code);
    throw std::runtime_error(
        description.empty() ?
            message :
            std::string{description} + ": " + message
    );
}

std::uint64_t effective_upload_rate(
    const http::UploadTransportOptions& options
) {
    const auto per_file = options.maximum_send_speed_bytes_per_second;
    const auto total = options.maximum_total_send_speed_bytes_per_second;
    if (per_file == 0) {
        return total;
    }
    if (total == 0) {
        return per_file;
    }
    return std::min(per_file, total);
}

Json parse_graph_json(
    const http::HttpResponse& response,
    std::string_view description
) {
    try {
        return Json::parse(response.body);
    } catch (const Json::exception& error) {
        throw std::runtime_error(
            "Microsoft Graph returned invalid" +
            (description.empty() ?
                 std::string{} :
                 " " + std::string{description}) +
            " JSON: " + error.what()
        );
    }
}

std::optional<std::chrono::seconds> retry_after(
    const http::HttpResponse& response
) {
    for (const auto& header : response.headers) {
        if (!onedrive::util::ascii_iequals(
                header.name,
                "Retry-After"
            )) {
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
        if (onedrive::util::ascii_iequals(header.name, name)) {
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
    static_cast<void>(util::parse_remote_modified_time(timestamp));
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

std::optional<util::FileHash> item_content_hash(const Json& value) {
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
        return util::FileHash{
            .algorithm = util::FileHashAlgorithm::sha256,
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
        return util::FileHash{
            .algorithm = util::FileHashAlgorithm::quick_xor,
            .value = std::move(quick_xor.value()),
        };
    }
    return std::nullopt;
}

std::string item_ctag(const Json& item) {
    const auto ctag = item.find("cTag");
    if (ctag == item.end() || ctag->is_null()) {
        return {};
    }
    if (!ctag->is_string()) {
        throw std::runtime_error(
            "Microsoft Graph returned an invalid cTag"
        );
    }
    return ctag->get<std::string>();
}

RemoteItem parse_drive_item(
    const Json& json,
    std::string_view description,
    ContentValidation validation,
    DriveItemKind kind
) {
    try {
        RemoteItem item{
            .id = json.at("id").get<std::string>(),
            .name = json.at("name").get<std::string>(),
            .etag = json.at("eTag").get<std::string>(),
            .ctag = item_ctag(json),
            .parent_id = {},
            .remote_path = {},
            .last_modified = {},
            .size = 0,
            .directory = json.contains("folder"),
            .deleted = json.contains("deleted"),
            .root = json.contains("root"),
            .malware = item_is_malware(json),
            .content_hash = item_content_hash(json),
            .validate_content = validation == ContentValidation::strict,
        };
        item.remote_path = item_remote_path(json, item.name);
        if (const auto parent = json.find("parentReference");
            parent != json.end() && parent->is_object()) {
            if (const auto parent_id = parent->find("id");
                parent_id != parent->end() && parent_id->is_string()) {
                item.parent_id = parent_id->get<std::string>();
            }
        }
        if (const auto size = json.find("size");
            size != json.end() && size->is_number_integer()) {
            item.size = size->get<std::int64_t>();
        }
        if (auto modified = authoritative_last_modified(json);
            modified.has_value()) {
            item.last_modified = std::move(modified.value());
        }
        if (item.id.empty() || item.name.empty() || item.etag.empty() ||
            item.remote_path.empty() || item.deleted || item.root ||
            (item.directory && kind == DriveItemKind::file_only) ||
            item.size < 0 ||
            item.last_modified.empty()) {
            throw std::runtime_error(
                "Microsoft Graph returned invalid " +
                std::string{description} + " metadata"
            );
        }
        return item;
    } catch (const Json::exception& error) {
        throw std::runtime_error(
            "Microsoft Graph " + std::string{description} +
            " is missing required drive item data: " + error.what()
        );
    }
}

}  // namespace onedrive::graph::client_detail
