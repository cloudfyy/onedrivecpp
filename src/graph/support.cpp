#include "graph/support.hpp"
#include "graph/unsupported_formats.hpp"
#include "graph/delta_changes.hpp"

#include "onedrive/util/remote_time.hpp"
#include "util/ascii.hpp"
#include "util/uri.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <stdexcept>
#include <unordered_map>
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
    if (status_code == http::status::insufficient_storage) {
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
    return http::status::is_success(status_code);
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
    if (status_code == http::status::unauthorized) {
        throw AccountAuthenticationError{
            description.empty() ? message
                                : std::string{description} + ": " + message
        };
    }
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

http::HttpRequest with_upload_transfer_options(
    http::HttpRequest request, const http::UploadTransportOptions& options
) {
    const auto& transfer = options.transfer;
    request.connect_timeout = transfer.connect_timeout;
    request.operation_timeout = transfer.operation_timeout;
    request.low_speed_timeout = transfer.low_speed_timeout;
    request.low_speed_limit_bytes_per_second =
        transfer.low_speed_limit_bytes_per_second;
    request.http_version = transfer.http_version;
    request.ip_version = transfer.ip_version;
    request.maximum_response_size = maximum_json_response_bytes;
    return request;
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
    return status_code == http::status::request_timeout ||
           status_code == http::status::too_many_requests ||
           status_code == http::status::bad_gateway ||
           status_code == http::status::service_unavailable ||
           status_code == http::status::gateway_timeout;
}

bool stale_download_url_status(long status_code) {
    return status_code == http::status::unauthorized ||
           status_code == http::status::forbidden;
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

std::optional<UnsupportedItem> item_unsupported_format(const Json& value) {
    if (const auto package = value.find("package"); package != value.end()) {
        if (!package->is_object()) {
            throw std::runtime_error{
                "Microsoft Graph returned an invalid package facet"
            };
        }
        if (const auto type = package->find("type"); type != package->end()) {
            if (!type->is_string()) {
                throw std::runtime_error{
                    "Microsoft Graph returned an invalid package type"
                };
            }
            for (const auto& rule : unsupported_format_rules) {
                if (std::ranges::find(
                        rule.package_types, type->get_ref<const std::string&>()
                    ) != rule.package_types.end()) {
                    return UnsupportedItem{rule.id, true};
                }
            }
        }
    }
    if (const auto* remote = remote_item_facet(value)) {
        const auto kind = item_unsupported_format(*remote);
        if (kind) {
            return kind;
        }
    }
    const auto file = value.find("file");
    if (file == value.end()) {
        return std::nullopt;
    }
    if (!file->is_object()) {
        throw std::runtime_error{
            "Microsoft Graph returned an invalid file facet"
        };
    }
    const auto mime = file->find("mimeType");
    if (mime == file->end()) {
        return std::nullopt;
    }
    if (!mime->is_string()) {
        throw std::runtime_error{
            "Microsoft Graph returned an invalid file mimeType"
        };
    }
    const auto& mime_type = mime->get_ref<const std::string&>();
    const auto name = value.find("name");
    if (name == value.end() || !name->is_string()) {
        return std::nullopt;
    }
    const std::string_view filename = name->get_ref<const std::string&>();
    const auto dot = filename.rfind('.');
    const auto extension = dot == std::string_view::npos ? std::string_view{}
                                                         : filename.substr(dot);
    for (const auto& rule : unsupported_format_rules) {
        if (std::ranges::any_of(
                rule.mime_types,
                [&](std::string_view mime) {
                    return util::ascii_iequals(mime_type, mime);
                }
            ) &&
            std::ranges::any_of(rule.extensions, [&](std::string_view suffix) {
                return util::ascii_iequals(extension, suffix);
            })) {
            return UnsupportedItem{rule.id, false};
        }
    }
    return std::nullopt;
}

Json unsupported_format_policy() {
    auto result = Json::array();
    for (const auto& rule : unsupported_format_rules) {
        result.push_back({
            {"id", rule.id},
            {"display_name", rule.display_name},
            {"mime_types",
             std::vector<std::string_view>{
                 rule.mime_types.begin(), rule.mime_types.end()
             }},
            {"extensions",
             std::vector<std::string_view>{
                 rule.extensions.begin(), rule.extensions.end()
             }},
            {"package_types",
             std::vector<std::string_view>{
                 rule.package_types.begin(), rule.package_types.end()
             }},
            {"ancestor_probe_directories",
             std::vector<std::string_view>{
                 rule.ancestor_probe_directories.begin(),
                 rule.ancestor_probe_directories.end()
             }},
        });
    }
    return result;
}

std::string unsupported_format_message(std::string_view id) {
    const auto rule = std::ranges::find(
        unsupported_format_rules, id, &UnsupportedFormatRule::id
    );
    if (rule == unsupported_format_rules.end()) {
        throw std::logic_error{
            "unknown unsupported format: " + std::string{id}
        };
    }
    return std::format(
        "{} items are unsupported; skipped without modifying existing local "
        "files",
        rule->display_name
    );
}

void mark_unsupported_descendants(
    std::vector<RemoteItem>& items,
    std::span<const RemoteItem> known_directories
) {
    if (!known_directories.empty()) {
        DeltaChanges merged;
        for (auto directory : known_directories) {
            // Saved paths are lookup hints; only IDs prove cached ancestry.
            if (!directory.unsupported_format.empty()) {
                directory.remote_path.clear();
            }
            merged.upsert(std::move(directory));
        }
        for (const auto& item : items) {
            merged.upsert(item);
        }
        auto hierarchy = std::move(merged).release();
        mark_unsupported_descendants(hierarchy);
        std::unordered_map<std::string, std::string> formats;
        for (const auto& item : hierarchy) {
            formats.emplace(item.id, item.unsupported_format);
        }
        for (auto& item : items) {
            item.unsupported_format = formats.at(item.id);
        }
        return;
    }
    if (std::ranges::none_of(items, [](const RemoteItem& item) {
            return !item.unsupported_format.empty() && item.directory;
        })) {
        return;
    }
    std::vector<std::size_t> directories;
    std::unordered_multimap<std::string, std::size_t> children;
    std::unordered_set<std::string> known_ids;
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        known_ids.insert(item.id);
        if (!item.unsupported_format.empty() && item.directory) {
            directories.push_back(index);
        }
        if (!item.parent_id.empty()) {
            children.emplace(item.parent_id, index);
        }
    }
    const auto initial_directories = directories;
    for (std::size_t index = 0; index < items.size(); ++index) {
        auto& item = items[index];
        if (!item.unsupported_format.empty() ||
            known_ids.contains(item.parent_id)) {
            continue;
        }
        for (const auto parent_index : initial_directories) {
            const auto& path = items[parent_index].remote_path;
            if (!path.empty() && item.remote_path.starts_with(path) &&
                item.remote_path.size() > path.size() &&
                item.remote_path[path.size()] == '/') {
                item.unsupported_format =
                    items[parent_index].unsupported_format;
                if (item.directory) {
                    directories.push_back(index);
                }
                break;
            }
        }
    }
    for (std::size_t cursor = 0; cursor < directories.size(); ++cursor) {
        const auto [begin, end] =
            children.equal_range(items[directories[cursor]].id);
        for (auto child = begin; child != end; ++child) {
            auto& item = items[child->second];
            if (item.unsupported_format.empty()) {
                item.unsupported_format =
                    items[directories[cursor]].unsupported_format;
                if (item.directory) {
                    directories.push_back(child->second);
                }
            }
        }
    }
}

RemoteItem parse_item_ancestry(const Json& json) {
    try {
        const auto unsupported = item_unsupported_format(json);
        RemoteItem item;
        item.id = json.at("id").get<std::string>();
        item.name = json.at("name").get<std::string>();
        item.root = json.contains("root");
        item.deleted = json.contains("deleted");
        item.directory = item.root || json.contains("folder") ||
                         (unsupported && unsupported->package);
        if (unsupported) {
            item.unsupported_format = unsupported->format_id;
        }
        if (const auto parent = json.find("parentReference");
            parent != json.end()) {
            if (!parent->is_object()) {
                throw std::runtime_error{
                    "Microsoft Graph returned invalid ancestry parent metadata"
                };
            }
            if (const auto id = parent->find("id"); id != parent->end()) {
                item.parent_id = id->get<std::string>();
            }
        }
        if (!item.root) {
            item.remote_path = item_remote_path(json, item.name);
        }
        if (item.id.empty() || item.name.empty() || item.deleted) {
            throw std::runtime_error{
                "Microsoft Graph returned invalid ancestry metadata"
            };
        }
        return item;
    } catch (const Json::exception& error) {
        throw std::runtime_error{
            "Microsoft Graph returned invalid ancestry metadata: " +
            std::string{error.what()}
        };
    }
}

void resolve_unsupported_ancestry(
    std::vector<RemoteItem>& items,
    std::span<const RemoteItem> known_directories,
    const std::function<RemoteItem(const std::string&)>& lookup,
    std::stop_token stop_token
) {
    if (stop_token.stop_requested()) {
        throw RequestCancelledError{
            "Microsoft Graph ancestry lookup was cancelled"
        };
    }
    if (items.empty()) {
        return;
    }
    mark_unsupported_descendants(items, known_directories);
    std::unordered_map<std::string, RemoteItem> ancestors;
    std::vector<std::string_view> saved_paths;
    for (const auto& directory : known_directories) {
        ancestors.insert_or_assign(directory.id, directory);
        if (!directory.unsupported_format.empty() &&
            !directory.remote_path.empty()) {
            saved_paths.push_back(directory.remote_path);
        }
    }
    for (const auto& item : items) {
        ancestors.insert_or_assign(item.id, item);
    }
    std::vector<std::string> fetched_ids;
    for (const auto& item : items) {
        if (stop_token.stop_requested()) {
            throw RequestCancelledError{
                "Microsoft Graph ancestry lookup was cancelled"
            };
        }
        if (item.deleted || !ancestors.at(item.id).unsupported_format.empty()) {
            continue;
        }
        const bool saved_path_hint =
            std::ranges::any_of(saved_paths, [&](std::string_view path) {
                return item.remote_path.starts_with(path) &&
                       item.remote_path.size() > path.size() &&
                       item.remote_path[path.size()] == '/';
            });
        const bool needs_probe =
            saved_path_hint ||
            std::ranges::any_of(
                unsupported_format_rules, [&](const auto& rule) {
                    return std::ranges::any_of(
                        rule.ancestor_probe_directories,
                        [&](std::string_view name) {
                            std::string_view path = item.remote_path;
                            while (!path.empty()) {
                                const auto separator = path.find('/');
                                if (util::ascii_iequals(
                                        path.substr(0, separator), name
                                    )) {
                                    return true;
                                }
                                if (separator == std::string_view::npos) {
                                    break;
                                }
                                path.remove_prefix(separator + 1);
                            }
                            return false;
                        }
                    );
                }
            );
        if (!needs_probe) {
            continue;
        }
        std::unordered_set<std::string> visited;
        const RemoteItem* current = &ancestors.at(item.id);
        while (!current->root && current->unsupported_format.empty()) {
            if (stop_token.stop_requested()) {
                throw RequestCancelledError{
                    "Microsoft Graph ancestry lookup was cancelled"
                };
            }
            if (!visited.insert(current->id).second) {
                throw std::runtime_error{
                    "Microsoft Graph returned a cyclic ancestry for '" +
                    item.remote_path + "'"
                };
            }
            auto parent = ancestors.find(current->parent_id);
            if (parent == ancestors.end()) {
                if (current->parent_id.empty()) {
                    throw std::runtime_error{
                        "Microsoft Graph ancestry is missing a parent ID for "
                        "'" +
                        current->remote_path + "'"
                    };
                }
                const auto parent_id = current->parent_id;
                auto fetched = lookup(parent_id);
                if (fetched.id != parent_id || !fetched.directory ||
                    fetched.deleted) {
                    throw std::runtime_error{
                        "Microsoft Graph returned an invalid ancestor for '" +
                        item.remote_path + "'"
                    };
                }
                fetched_ids.push_back(parent_id);
                parent = ancestors.emplace(parent_id, std::move(fetched)).first;
            }
            if (parent->second.deleted || !parent->second.directory) {
                throw std::runtime_error{
                    "Microsoft Graph returned an invalid ancestor for '" +
                    item.remote_path + "'"
                };
            }
            current = &parent->second;
        }
    }
    auto hierarchy = items;
    for (const auto& id : fetched_ids) {
        hierarchy.push_back(ancestors.at(id));
    }
    mark_unsupported_descendants(hierarchy, known_directories);
    std::unordered_map<std::string, std::string> formats;
    for (const auto& item : hierarchy) {
        formats.emplace(item.id, item.unsupported_format);
    }
    for (auto& item : items) {
        item.unsupported_format = formats.at(item.id);
    }
    for (const auto& id : fetched_ids) {
        const auto found = std::ranges::find(hierarchy, id, &RemoteItem::id);
        items.push_back(*found);
    }
}

std::optional<util::FileHash>
item_content_hash(const Json& value, ContentValidation validation) {
    if (item_unsupported_format(value)) {
        return std::nullopt;
    }
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
    const auto read_hash =
        [&](std::string_view name) -> std::optional<std::string> {
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
    if (auto quick_xor = read_hash("quickXorHash");
        quick_xor.has_value()) {
        constexpr std::size_t quick_xor_base64_characters = 28;
        const bool valid =
            quick_xor.value().size() == quick_xor_base64_characters &&
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
    if (auto sha1 = read_hash("sha1Hash"); sha1.has_value()) {
        constexpr std::size_t sha1_hex_characters = 40;
        if (sha1->size() != sha1_hex_characters ||
            !std::ranges::all_of(*sha1, [](unsigned char character) {
                return std::isxdigit(character) != 0;
            })) {
            throw std::runtime_error(
                "Microsoft Graph returned an invalid sha1Hash"
            );
        }
        return util::FileHash{
            .algorithm = util::FileHashAlgorithm::sha1,
            .value = std::move(sha1.value()),
        };
    }
    if (hashes->contains("sha256Hash") &&
        validation == ContentValidation::strict) {
        throw std::runtime_error(
            std::format(
                "Microsoft Graph returned unsupported sha256Hash without "
                "quickXorHash or sha1Hash for drive item '{}' ('{}'); "
                "cannot verify the remote content hash",
                value.value("id", std::string{}),
                value.value("name", std::string{})
            )
        );
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
    DriveItemKind kind,
    std::string_view inherited_unsupported_format
) {
    try {
        const auto unsupported = item_unsupported_format(json);
        RemoteItem item{
            .id = json.at("id").get<std::string>(),
            .name = json.at("name").get<std::string>(),
            .etag = json.at("eTag").get<std::string>(),
            .ctag = item_ctag(json),
            .parent_id = {},
            .remote_path = {},
            .last_modified = {},
            .size = 0,
            .directory = json.contains("folder") ||
                         (unsupported && unsupported->package),
            .deleted = json.contains("deleted"),
            .root = json.contains("root"),
            .malware = item_is_malware(json),
            .content_hash = inherited_unsupported_format.empty()
                                ? item_content_hash(json, validation)
                                : std::nullopt,
            .validate_content = validation == ContentValidation::strict,
            .unsupported_format =
                !inherited_unsupported_format.empty()
                    ? std::string{inherited_unsupported_format}
                : unsupported ? std::string{unsupported->format_id}
                              : "",
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
            (item.directory && kind == DriveItemKind::file_only &&
             item.unsupported_format.empty()) ||
            item.size < 0 ||
            (item.last_modified.empty() && item.unsupported_format.empty())) {
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
