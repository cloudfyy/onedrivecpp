#include "graph/support.hpp"
#include "graph/unsupported_formats.hpp"
#include "support/common.hpp"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace detail = onedrive::graph::client_detail;
using detail::Json;
using onedrive::test::fail;

using onedrive::test::throws_with;

struct MoveOnlyPageFetcher {
    std::unique_ptr<std::size_t> calls = std::make_unique<std::size_t>(0);

    Json operator()(const std::string& url) & {
        ++*calls;
        if (*calls == 1 && url == "https://graph.example.test/page1") {
            return {
                {"value", Json::array({1})},
                {"@odata.nextLink", "https://graph.example.test/page2"},
            };
        }
        if (*calls == 2 && url == "https://graph.example.test/page2") {
            return {{"value", Json::array({2})}};
        }
        throw std::runtime_error{"pagination callable was reused incorrectly"};
    }
    Json operator()(const std::string&) && = delete;
};

int test_unsupported_formats() {
    for (const auto& rule : detail::unsupported_format_rules) {
        for (const auto mime : rule.mime_types) {
            for (const auto extension : rule.extensions) {
                const Json value{
                    {"name", "notes" + std::string{extension}},
                    {"file",
                     {{"mimeType", mime},
                      {"hashes", {{"sha256Hash", "unsupported"}}}}}
                };
                const auto found = detail::item_unsupported_format(value);
                if (!found || found->format_id != rule.id || found->package ||
                    detail::item_content_hash(value) ||
                    !detail::unsupported_format_message(rule.id).contains(
                        rule.display_name
                    )) {
                    return fail(
                        "built-in format rule was not applied consistently"
                    );
                }
                const auto shortcut = detail::item_unsupported_format(
                    Json{{"name", "shortcut"}, {"remoteItem", value}}
                );
                if (!shortcut || shortcut->format_id != rule.id) {
                    return fail("shared unsupported format was not recognized");
                }
            }
        }
        for (const auto package : rule.package_types) {
            const auto found = detail::item_unsupported_format(
                Json{{"package", {{"type", package}}}}
            );
            if (!found || found->format_id != rule.id || !found->package) {
                return fail("configured package type was not recognized");
            }
        }
    }
    const std::vector<Json> ordinary{
        {{"name", "notes.one"}, {"file", {{"mimeType", "text/plain"}}}},
        {{"name", "data.bin"},
         {"file", {{"mimeType", "application/octet-stream"}}}},
        {{"name", "notes.one"}, {"file", Json::object()}},
        {{"name", "notes.one.zip"},
         {"file", {{"mimeType", "application/msonenote"}}}},
        {{"name", "notes.txt"},
         {"file", {{"mimeType", "application/msonenote"}}}},
        {{"package", {{"type", "other"}}}},
    };
    for (const auto& value : ordinary) {
        if (detail::item_unsupported_format(value)) {
            return fail("extension or MIME alone excluded an ordinary file");
        }
    }
    const auto upper = detail::item_unsupported_format(
        Json{
            {"name", "NOTES.ONETOC2"},
            {"file", {{"mimeType", "APPLICATION/MSONENOTE"}}}
        }
    );
    if (!upper || upper->format_id != "onenote" ||
        !throws_with<std::runtime_error>(
            [] {
                static_cast<void>(detail::item_unsupported_format(
                    Json{{"file", {{"mimeType", 42}}}}
                ));
            },
            "invalid file mimeType"
        ) ||
        !throws_with<std::runtime_error>(
            [] {
                static_cast<void>(
                    detail::item_unsupported_format(Json{{"package", true}})
                );
            },
            "invalid package facet"
        ) ||
        !throws_with<std::runtime_error>(
            [] {
                static_cast<void>(detail::item_unsupported_format(
                    Json{{"package", {{"type", 42}}}}
                ));
            },
            "invalid package type"
        ) ||
        !throws_with<std::logic_error>(
            [] {
                static_cast<void>(
                    detail::unsupported_format_message("unknown")
                );
            },
            "unknown unsupported format"
        )) {
        return fail(
            "format rules lost case handling or explicit invalid-metadata "
            "errors"
        );
    }
    const auto policy = detail::unsupported_format_policy();
    if (!policy.is_array() ||
        policy.size() != detail::unsupported_format_rules.size() ||
        policy[0]["id"] != "onenote" || policy[0]["mime_types"].size() != 2 ||
        policy[0]["extensions"].size() != 2 ||
        policy[0]["package_types"] != Json::array({"oneNote"}) ||
        policy[0]["ancestor_probe_directories"] !=
            Json::array({"OneNote_RecycleBin"})) {
        return fail("metadata cache policy did not capture the built-in rules");
    }
    return EXIT_SUCCESS;
}

int test_ancestor_metadata_validation() {
    const Json folder{
        {"id", "folder"},
        {"name", "Folder"},
        {"folder", Json::object()},
        {"parentReference", {{"id", "root"}, {"path", "/drive/root:"}}},
    };
    const auto parsed = detail::parse_item_ancestry(folder);
    if (parsed.id != "folder" || parsed.name != "Folder" ||
        parsed.parent_id != "root" || parsed.remote_path != "Folder" ||
        !parsed.directory || parsed.root || parsed.content_hash ||
        !parsed.unsupported_format.empty()) {
        return fail("ancestor metadata lost the ID, path, or directory type");
    }
    std::vector<Json> invalid;
    for (const auto field : {"id", "name"}) {
        auto missing = folder;
        missing.erase(field);
        invalid.push_back(missing);
        auto empty = folder;
        empty[field] = "";
        invalid.push_back(empty);
    }
    auto deleted = folder;
    deleted["deleted"] = Json::object();
    invalid.push_back(deleted);
    auto invalid_parent = folder;
    invalid_parent["parentReference"] = "not an object";
    invalid.push_back(invalid_parent);
    invalid_parent = folder;
    invalid_parent["parentReference"]["id"] = 42;
    invalid.push_back(invalid_parent);
    for (const auto& value : invalid) {
        if (!throws_with<std::runtime_error>(
                [&] { static_cast<void>(detail::parse_item_ancestry(value)); },
                "invalid ancestry"
            )) {
            return fail(
                "invalid ancestor JSON was accepted or hid its context"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_ancestor_failure_and_cancellation_boundaries() {
    namespace graph = onedrive::graph;
    const graph::RemoteItem recycle{
        .id = "recycle",
        .name = "OneNote_RecycleBin",
        .parent_id = "parent",
        .remote_path = "Notebook/OneNote_RecycleBin",
        .directory = true,
    };
    const graph::RemoteItem child{
        .id = "child",
        .name = "data.bin",
        .parent_id = "recycle",
        .remote_path = "Notebook/OneNote_RecycleBin/data.bin",
    };
    const graph::RemoteItem parent{
        .id = "parent",
        .name = "Notebook",
        .parent_id = "root",
        .remote_path = "Notebook",
        .directory = true,
    };
    for (const bool cached : {false, true}) {
        for (const bool deleted : {false, true}) {
            auto invalid = parent;
            invalid.deleted = deleted;
            invalid.directory = deleted;
            auto items = std::vector{child, recycle};
            if (cached) {
                items.push_back(invalid);
            }
            int lookups = 0;
            if (!throws_with<std::runtime_error>(
                    [&] {
                        detail::resolve_unsupported_ancestry(
                            items, {}, [&](const std::string&) {
                                ++lookups;
                                return invalid;
                            }
                        );
                    },
                    "invalid ancestor"
                ) ||
                lookups != (cached ? 0 : 1)) {
                return fail(
                    "a file/deleted ancestor was accepted or re-queried"
                );
            }
        }
    }
    for (const bool during_walk : {false, true}) {
        std::stop_source stop;
        auto items = std::vector{child, recycle};
        int lookups = 0;
        if (!throws_with<graph::RequestCancelledError>(
                [&] {
                    detail::resolve_unsupported_ancestry(
                        items,
                        {},
                        [&](const std::string&) {
                            ++lookups;
                            stop.request_stop();
                            auto result = parent;
                            if (!during_walk) {
                                result.unsupported_format = "onenote";
                            }
                            return result;
                        },
                        stop.get_token()
                    );
                },
                "cancelled"
            ) ||
            lookups != 1 || items.size() != 2 ||
            std::ranges::any_of(items, [](const auto& item) {
                return !item.unsupported_format.empty();
            })) {
            return fail(
                "cancelled ancestry lookup continued or installed results"
            );
        }
    }
    auto package = parent;
    package.unsupported_format = "onenote";
    auto path_only = recycle;
    path_only.parent_id.clear();
    auto items = std::vector{child, path_only, package};
    detail::mark_unsupported_descendants(items);
    if (std::ranges::any_of(items, [](const auto& item) {
            return item.unsupported_format != "onenote";
        })) {
        return fail(
            "a path-confirmed directory did not protect its ID-linked child"
        );
    }
    return EXIT_SUCCESS;
}

int test_unsupported_ancestry() {
    namespace graph = onedrive::graph;
    graph::RemoteItem package{
        .id = "package",
        .name = "Notebook",
        .parent_id = "root",
        .remote_path = "Notebook",
        .directory = true,
        .unsupported_format = "onenote",
    };
    graph::RemoteItem middle{
        .id = "middle",
        .name = "Sub",
        .parent_id = "package",
        .remote_path = "Notebook/Sub",
        .directory = true,
    };
    graph::RemoteItem recycle{
        .id = "recycle",
        .name = "OneNote_RecycleBin",
        .parent_id = "middle",
        .remote_path = "Notebook/Sub/OneNote_RecycleBin",
        .directory = true,
    };
    graph::RemoteItem child{
        .id = "child",
        .name = "data.bin",
        .parent_id = "recycle",
        .remote_path = "Notebook/Sub/OneNote_RecycleBin/data.bin",
    };
    const auto no_lookup = [](const std::string&) -> graph::RemoteItem {
        throw std::runtime_error{"unexpected ancestor lookup"};
    };
    auto saved_middle = middle;
    saved_middle.unsupported_format = "onenote";
    const std::vector context{package, saved_middle};
    auto items = std::vector{child, recycle};
    detail::resolve_unsupported_ancestry(items, context, no_lookup);
    if (items.size() != 2 || std::ranges::any_of(items, [](const auto& item) {
            return item.unsupported_format != "onenote";
        })) {
        return fail("stored ancestry did not protect a partial OneNote delta");
    }

    std::vector<std::string> lookups;
    items = {child, recycle};
    detail::resolve_unsupported_ancestry(items, {}, [&](const std::string& id) {
        lookups.push_back(id);
        return id == middle.id ? middle : package;
    });
    if (lookups != std::vector<std::string>{"middle", "package"} ||
        items.size() != 4 || std::ranges::any_of(items, [](const auto& item) {
            return item.unsupported_format != "onenote";
        })) {
        return fail(
            "ancestor lookups were not cached or discovered blockers lost"
        );
    }

    auto documents = package;
    documents.id = "documents";
    documents.name = documents.remote_path = "Documents";
    documents.unsupported_format.clear();
    graph::RemoteItem root{
        .id = "root", .name = "Drive", .directory = true, .root = true
    };
    middle.parent_id = documents.id;
    middle.remote_path = "Documents/Sub";
    recycle.remote_path = "Documents/Sub/OneNote_RecycleBin";
    child.remote_path = recycle.remote_path + "/data.bin";
    items = {child, recycle, middle, documents, root};
    detail::resolve_unsupported_ancestry(items, context, no_lookup);
    if (std::ranges::any_of(items, [](const auto& item) {
            return !item.unsupported_format.empty();
        })) {
        return fail("a moved ancestor retained its old OneNote association");
    }

    items = {child, recycle};
    detail::resolve_unsupported_ancestry(
        items, std::vector{middle, documents, root}, no_lookup
    );
    if (!items[0].unsupported_format.empty() ||
        !items[1].unsupported_format.empty()) {
        return fail("an ordinary same-name directory was blocked");
    }
    auto replacement = documents;
    replacement.id = "replacement";
    replacement.name = replacement.remote_path = "Notebook";
    auto reused_path = child;
    reused_path.parent_id = replacement.id;
    reused_path.remote_path = "Notebook/new.bin";
    items = {reused_path};
    detail::resolve_unsupported_ancestry(
        items, context, [&](const std::string& id) {
            return id == replacement.id ? replacement : root;
        }
    );
    items.push_back(package);
    detail::mark_unsupported_descendants(items, context);
    if (items.size() != 4 || !items.front().unsupported_format.empty() ||
        !items[1].unsupported_format.empty()) {
        return fail("a saved path overrode newly confirmed ancestor IDs");
    }
    for (const bool invalid_id : {false, true}) {
        auto invalid = middle;
        if (invalid_id) {
            invalid.id = "wrong-drive-or-id";
        } else {
            invalid.parent_id = recycle.id;
        }
        items = {child, recycle};
        if (!throws_with<std::runtime_error>(
                [&] {
                    detail::resolve_unsupported_ancestry(
                        items, {}, [&](const std::string&) { return invalid; }
                    );
                },
                invalid_id ? "invalid ancestor" : "cyclic ancestry"
            )) {
            return fail("invalid ancestor identity or cycle was accepted");
        }
    }
    items = {child};
    if (!throws_with<std::runtime_error>(
            [&] {
                detail::resolve_unsupported_ancestry(
                    items, {}, [](const std::string&) -> graph::RemoteItem {
                        throw std::runtime_error{"ancestor lookup failed"};
                    }
                );
            },
            "ancestor lookup failed"
        )) {
        return fail("ancestor lookup failure silently allowed a download");
    }
    child.parent_id.clear();
    items = {child};
    if (!throws_with<std::runtime_error>(
            [&] { detail::resolve_unsupported_ancestry(items, {}, no_lookup); },
            "missing a parent ID"
        )) {
        return fail("missing ancestry metadata silently allowed a download");
    }
    std::stop_source stopped;
    stopped.request_stop();
    if (!throws_with<graph::RequestCancelledError>(
            [&] {
                detail::resolve_unsupported_ancestry(
                    items, {}, no_lookup, stopped.get_token()
                );
            },
            "cancelled"
        )) {
        return fail("ancestor lookup ignored cancellation");
    }
    return EXIT_SUCCESS;
}

int test_repeated_page_fetcher() {
    const auto fetch = [](auto operation) {
        return detail::paged_graph_values(
            "https://graph.example.test/page1",
            "https://graph.example.test",
            "test pages",
            std::move(operation)
        );
    };
    if (fetch(MoveOnlyPageFetcher{}) != std::vector<Json>{1, 2}) {
        return fail("owned move-only pagination callable lost its state");
    }
    MoveOnlyPageFetcher borrowed;
    if (fetch(std::ref(borrowed)) != std::vector<Json>{1, 2} ||
        *borrowed.calls != 2) {
        return fail("borrowed pagination callable lost its state");
    }
    std::size_t calls = 0;
    if (!throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(fetch([&](const std::string&) -> Json {
                    if (++calls == 2) {
                        throw std::runtime_error{"second page failed"};
                    }
                    return {
                        {"value", Json::array({1})},
                        {"@odata.nextLink", "https://graph.example.test/page2"},
                    };
                }));
            },
            "second page failed"
        ) ||
        calls != 2) {
        return fail("pagination swallowed or retried a fetch failure");
    }
    return EXIT_SUCCESS;
}

int test_paths_and_errors() {
    if (detail::percent_encode_remote_path("My Folder/report #1.txt") !=
            "My%20Folder/report%20%231.txt" ||
        detail::normalized_endpoint("https://graph.example.test///") !=
            "https://graph.example.test" ||
        detail::graph_drive_prefix({
            .drive_id = "me",
            .endpoint = "https://graph.example.test",
        }) != "https://graph.example.test/me/drive" ||
        detail::graph_drive_prefix({
            .drive_id = "shared drive",
            .endpoint = "https://graph.example.test",
        }) != "https://graph.example.test/drives/shared%20drive") {
        return fail("Graph path or endpoint helpers returned the wrong value");
    }
    for (const std::string_view path : {
             "",
             "/absolute",
             "trailing/",
             "empty//segment",
             "./file",
             "../file",
             "directory\\file",
         }) {
        if (!throws_with(
                [&] {
                    static_cast<void>(detail::percent_encode_remote_path(path));
                },
                "remote file path"
            )) {
            return fail("unsafe Graph path was accepted");
        }
    }

    const Json error = {
        {"error", {{"code", "quotaLimitReached"}, {"message", "full"}}},
    };
    for (const auto* code : {
             "QUOTALIMITREACHED", "sToRaGeLiMiTeXcEeDeD", "INSUFFICIENTSTORAGE",
         }) {
        const Json response{{"error", {{"code", code}}}};
        if (!detail::upload_quota_error(response, 403) ||
            detail::graph_error_code(response) != code) {
            return fail("Graph quota comparison changed or rejected its code");
        }
    }
    for (const auto* code : {
             "quotaLimitReachedExtra", " quotaLimitReached",
             "insufficientStorage ", "\xc4", "",
         }) {
        if (detail::upload_quota_error(
                Json{{"error", {{"code", code}}}}, 403
            )) {
            return fail("non-quota Graph code was classified as quota");
        }
    }
    if (detail::graph_error_message(error, 400) != "full" ||
        detail::graph_error_code(error) != "quotaLimitReached" ||
        detail::graph_error_message(Json::object(), 418) !=
            "Microsoft Graph request failed with HTTP 418" ||
        !detail::graph_error_code(Json{{"error", "invalid"}}).empty() ||
        !detail::upload_quota_error(Json::object(), 507) ||
        !detail::upload_quota_error(error, 400) ||
        !detail::upload_quota_error(
            Json{{"error", {{"code", "StorageLimitExceeded"}}}}, 400
        ) ||
        !detail::upload_quota_error(
            Json{{"error", {{"code", "insufficientstorage"}}}}, 400
        ) ||
        detail::upload_quota_error(Json::object(), 400)) {
        return fail("Graph error helpers classified a response incorrectly");
    }
    try {
        detail::throw_upload_response_error(error, 400);
        return fail("Graph quota response was accepted");
    } catch (const onedrive::graph::UploadResourceError& resource_error) {
        if (resource_error.reason_code() != "remote_quota") {
            return fail("Graph quota response used the wrong error code");
        }
    }
    if (!throws_with(
            [&] { detail::throw_upload_response_error(Json::object(), 500); },
            "HTTP 500"
        )) {
        return fail("Graph upload server error was accepted");
    }
    return EXIT_SUCCESS;
}

int test_status_and_retry_helpers() {
    if (detail::successful_status(199) || !detail::successful_status(200) ||
        !detail::successful_status(299) || detail::successful_status(300) ||
        !detail::retryable_status(408) || !detail::retryable_status(429) ||
        !detail::retryable_status(502) || !detail::retryable_status(503) ||
        !detail::retryable_status(504) || detail::retryable_status(500) ||
        !detail::stale_download_url_status(401) ||
        !detail::stale_download_url_status(403) ||
        detail::stale_download_url_status(404)) {
        return fail("Graph status classification was incorrect");
    }
    detail::require_successful_graph_response(Json::object(), 204, "ignored");
    if (!throws_with(
            [&] {
                detail::require_successful_graph_response(Json::object(), 500);
            },
            "HTTP 500"
        ) ||
        !throws_with(
            [&] {
                detail::require_successful_graph_response(
                    Json::object(), 500, "listing files"
                );
            },
            "listing files: Microsoft Graph"
        )) {
        return fail("Graph failure response was accepted");
    }

    using onedrive::http::UploadTransportOptions;
    if (detail::effective_upload_rate({}) != 0 ||
        detail::effective_upload_rate(
            UploadTransportOptions{
                .maximum_send_speed_bytes_per_second = 10,
            }
        ) != 10 ||
        detail::effective_upload_rate(
            UploadTransportOptions{
                .maximum_total_send_speed_bytes_per_second = 20,
            }
        ) != 20 ||
        detail::effective_upload_rate(
            UploadTransportOptions{
                .maximum_send_speed_bytes_per_second = 30,
                .maximum_total_send_speed_bytes_per_second = 20,
            }
        ) != 20) {
        return fail("effective Graph upload rate was incorrect");
    }

    const onedrive::http::HttpResponse response{
        .headers = {
            {.name = "Unrelated", .value = "ignored"},
            {.name = "retry-after", .value = "3"},
            {.name = "Request-Id", .value = "request"},
        },
    };
    if (detail::retry_after(response) != std::chrono::seconds{3} ||
        detail::header_value(response, "request-id") != "request" ||
        detail::header_value(response, "missing") ||
        detail::retry_after({
            .headers = {{.name = "Retry-After", .value = "-1"}},
        }) ||
        detail::retry_after({
            .headers = {{.name = "Retry-After", .value = "3 seconds"}},
        })) {
        return fail("Graph response headers were parsed incorrectly");
    }

    const onedrive::graph::GraphOptions options{
        .initial_throttle_delay = std::chrono::seconds{3},
        .maximum_throttle_delay = std::chrono::seconds{10},
    };
    if (detail::fallback_retry_delay(options, 0) != std::chrono::seconds{3} ||
        detail::fallback_retry_delay(options, 1) != std::chrono::seconds{6} ||
        detail::fallback_retry_delay(options, 2) != std::chrono::seconds{10} ||
        detail::fallback_retry_delay(options, 20) != std::chrono::seconds{10}) {
        return fail("Graph fallback retry delay was incorrect");
    }

    int sleeps = 0;
    if (detail::wait_for_retry(
            std::chrono::seconds::zero(),
            [&](std::chrono::seconds) { ++sleeps; },
            {}
        ) ||
        sleeps != 1) {
        return fail("uncancellable Graph retry did not use the sleep callback");
    }
    std::stop_source stop;
    stop.request_stop();
    if (!detail::wait_for_retry(
            std::chrono::seconds{1},
            [](std::chrono::seconds) {},
            stop.get_token()
        )) {
        return fail("cancelled Graph retry wait was not interrupted");
    }
    const auto cancelled = detail::cancelled_http_result();
    if (cancelled ||
        cancelled.error().code != onedrive::http::HttpErrorCode::cancelled) {
        return fail("cancelled Graph HTTP result was incorrect");
    }
    return EXIT_SUCCESS;
}

int test_json_and_facets() {
    const auto parsed =
        detail::parse_graph_json({.body = R"({"value":1})"}, "test");
    if (parsed.at("value") != 1 ||
        !throws_with(
            [] {
                static_cast<void>(
                    detail::parse_graph_json({.body = "{"}, "test")
                );
            },
            "invalid test JSON"
        ) ||
        !throws_with(
            [] { static_cast<void>(detail::parse_graph_json({.body = "{"})); },
            "invalid JSON"
        )) {
        return fail("Graph JSON parser accepted invalid input");
    }

    if (detail::item_remote_path(Json::object(), "name.txt") != "name.txt" ||
        detail::item_remote_path(
            Json{{"parentReference", Json::object()}}, "name.txt"
        ) != "name.txt" ||
        detail::item_remote_path(
            Json{{
                "parentReference",
                {{"path", "/drives/id/items/parent"}},
            }},
            "name.txt"
        ) != "name.txt" ||
        detail::item_remote_path(
            Json{{"parentReference", {{"path", "/drives/id/root:"}}}},
            "name.txt"
        ) != "name.txt" ||
        detail::item_remote_path(
            Json{{
                "parentReference",
                {{"path", "/drives/id/root:///folder"}},
            }},
            "name.txt"
        ) != "folder/name.txt") {
        return fail("Graph item remote path was parsed incorrectly");
    }

    if (detail::file_system_last_modified(Json::object(), "item") ||
        detail::file_system_last_modified(
            Json{{"fileSystemInfo", Json::object()}}, "item"
        ) ||
        !throws_with(
            [] {
                static_cast<void>(detail::file_system_last_modified(
                    Json{{"fileSystemInfo", "invalid"}}, "item"
                ));
            },
            "invalid item fileSystemInfo"
        ) ||
        !throws_with(
            [] {
                static_cast<void>(detail::file_system_last_modified(
                    Json{{
                        "fileSystemInfo",
                        {{"lastModifiedDateTime", ""}},
                    }},
                    "item"
                ));
            },
            "fileSystemInfo.lastModifiedDateTime"
        )) {
        return fail("Graph filesystem timestamp facet was parsed incorrectly");
    }
    const Json timestamp{
        {
            "fileSystemInfo",
            {{"lastModifiedDateTime", "2026-10-07T00:00:00Z"}},
        },
    };
    if (detail::file_system_last_modified(timestamp, "item") !=
        "2026-10-07T00:00:00Z") {
        return fail("valid Graph filesystem timestamp was not returned");
    }

    if (detail::remote_item_facet(Json::object()) != nullptr ||
        !throws_with(
            [] {
                static_cast<void>(
                    detail::remote_item_facet(Json{{"remoteItem", "invalid"}})
                );
            },
            "invalid remoteItem facet"
        )) {
        return fail("Graph remote item facet was parsed incorrectly");
    }
    const Json remote_timestamp{
        {"fileSystemInfo", {{"lastModifiedDateTime", "2026-01-01T00:00:00Z"}}},
        {
            "remoteItem",
            {{
                "fileSystemInfo",
                {{"lastModifiedDateTime", "2026-02-01T00:00:00Z"}},
            }},
        },
    };
    if (detail::authoritative_last_modified(remote_timestamp) !=
        "2026-02-01T00:00:00Z") {
        return fail("Graph remote item timestamp was not authoritative");
    }

    if (detail::item_is_malware(Json::object()) ||
        !detail::item_is_malware(Json{{"malware", Json::object()}}) ||
        !detail::item_is_malware(
            Json{{"remoteItem", {{"malware", Json::object()}}}}
        ) ||
        !throws_with(
            [] {
                static_cast<void>(
                    detail::item_is_malware(Json{{"malware", true}})
                );
            },
            "invalid drive item malware facet"
        )) {
        return fail("Graph malware facet was parsed incorrectly");
    }
    return EXIT_SUCCESS;
}

int test_hashes_and_drive_items() {
    const std::string sha256(64, 'a');
    const std::string quick_xor = "AAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    if (detail::item_content_hash(Json::object()) ||
        detail::item_content_hash(Json{{"file", Json::object()}}) ||
        detail::item_content_hash(
            Json{{"file", {{"hashes", Json::object()}}}}
        ) ||
        !throws_with(
            [] {
                static_cast<void>(
                    detail::item_content_hash(Json{{"file", "invalid"}})
                );
            },
            "invalid file facet"
        ) ||
        !throws_with(
            [] {
                static_cast<void>(detail::item_content_hash(
                    Json{{"file", {{"hashes", "invalid"}}}}
                ));
            },
            "invalid file hash facet"
        ) ||
        !throws_with(
            [] {
                static_cast<void>(detail::item_content_hash(
                    Json{{
                        "file",
                        {{"hashes", {{"sha256Hash", "short"}}}},
                    }}
                ));
            },
            "unsupported sha256Hash without quickXorHash"
        ) ||
        !throws_with(
            [] {
                static_cast<void>(detail::item_content_hash(
                    Json{{
                        "file",
                        {{"hashes", {{"sha256Hash", nullptr}}}},
                    }}
                ));
            },
            "unsupported sha256Hash without quickXorHash"
        )) {
        return fail("invalid Graph file hash facet was accepted");
    }
    const auto combined = detail::item_content_hash(
        Json{{
            "file",
            {{"hashes", {{"sha256Hash", sha256}, {"quickXorHash", quick_xor}}}},
        }}
    );
    const auto quick = detail::item_content_hash(
        Json{{
            "file",
            {{"hashes", {{"quickXorHash", quick_xor}}}},
        }}
    );
    if (!combined || combined->value != quick_xor ||
        combined->algorithm != onedrive::util::FileHashAlgorithm::quick_xor ||
        !quick || quick->value != quick_xor) {
        return fail("valid Graph file hash was not parsed");
    }
    const std::string sha1{"A17C9AAA61E80A1BF71D0D850AF4E5BAA9800BBD"};
    for (const bool unsupported_present : {false, true}) {
        Json hashes{{"sha1Hash", sha1}};
        if (unsupported_present) {
            hashes["sha256Hash"] = sha256;
        }
        const auto parsed =
            detail::item_content_hash(Json{{"file", {{"hashes", hashes}}}});
        if (!parsed ||
            parsed->algorithm != onedrive::util::FileHashAlgorithm::sha1 ||
            parsed->value != sha1) {
            return fail(
                "supported Graph SHA-1 was not selected without QuickXorHash"
            );
        }
        hashes["quickXorHash"] = quick_xor;
        const auto preferred =
            detail::item_content_hash(Json{{"file", {{"hashes", hashes}}}});
        if (!preferred || preferred->algorithm !=
                              onedrive::util::FileHashAlgorithm::quick_xor) {
            return fail("SHA-1 replaced the preferred QuickXorHash");
        }
    }
    for (const auto& invalid_sha1 :
         {Json(""),
          Json(nullptr),
          Json(42),
          Json("short"),
          Json(std::string(40, 'g'))}) {
        if (!throws_with(
                [&] {
                    static_cast<void>(detail::item_content_hash(
                        Json{
                            {"file",
                             {{"hashes",
                               {
                                   {"sha1Hash", invalid_sha1},
                                   {"sha256Hash", sha256},
                               }}}},
                        }
                    ));
                },
                "invalid sha1Hash"
            )) {
            return fail(
                "invalid SHA-1 metadata fell back to unsupported SHA-256"
            );
        }
    }
    for (const auto& unsupported :
         {Json(sha256), Json("short"), Json(""), Json(nullptr), Json(42)}) {
        const Json document{
            {"file", {{"hashes", {{"sha256Hash", unsupported}}}}},
        };
        if (!throws_with(
                [&] { static_cast<void>(detail::item_content_hash(document)); },
                "unsupported sha256Hash without quickXorHash"
            ) ||
            detail::item_content_hash(
                document, detail::ContentValidation::relaxed
            )) {
            return fail("unsupported Graph SHA-256 was used as a content hash");
        }
        auto with_quick_xor = document;
        with_quick_xor["file"]["hashes"]["quickXorHash"] = quick_xor;
        const auto supported = detail::item_content_hash(with_quick_xor);
        if (!supported || supported->value != quick_xor ||
            supported->algorithm !=
                onedrive::util::FileHashAlgorithm::quick_xor) {
            return fail(
                "unsupported SHA-256 prevented supported hash validation"
            );
        }
        with_quick_xor["file"]["hashes"]["quickXorHash"] = "invalid=";
        if (!throws_with(
                [&] {
                    static_cast<void>(
                        detail::item_content_hash(with_quick_xor)
                    );
                },
                "invalid quickXorHash"
            )) {
            return fail(
                "invalid QuickXorHash fell back to unsupported SHA-256"
            );
        }
    }
    if (!throws_with(
            [] {
                static_cast<void>(detail::item_content_hash(
                    Json{{
                        "file",
                        {{"hashes", {{"quickXorHash", "invalid="}}}},
                    }}
                ));
            },
            "invalid quickXorHash"
        )) {
        return fail("invalid quick XOR hash was accepted");
    }

    if (!detail::item_ctag(Json::object()).empty() ||
        !detail::item_ctag(Json{{"cTag", nullptr}}).empty() ||
        detail::item_ctag(Json{{"cTag", "ctag"}}) != "ctag" ||
        !throws_with(
            [] { static_cast<void>(detail::item_ctag(Json{{"cTag", 1}})); },
            "invalid cTag"
        )) {
        return fail("Graph cTag was parsed incorrectly");
    }

    Json item{
        {"id", "item-id"},
        {"name", "file.txt"},
        {"eTag", "etag"},
        {"cTag", "ctag"},
        {
            "parentReference",
            {
                {"id", "parent-id"},
                {"path", "/drives/id/root:/folder"},
            },
        },
        {"size", 4},
        {"file",
         {{"hashes", {{"sha256Hash", sha256}, {"quickXorHash", quick_xor}}}}},
        {
            "fileSystemInfo",
            {{"lastModifiedDateTime", "2026-10-07T00:00:00Z"}},
        },
    };
    const auto parsed = detail::parse_drive_item(
        item,
        "drive item",
        detail::ContentValidation::strict,
        detail::DriveItemKind::file_only
    );
    if (parsed.id != "item-id" || parsed.parent_id != "parent-id" ||
        parsed.remote_path != "folder/file.txt" || parsed.size != 4 ||
        !parsed.content_hash || !parsed.validate_content) {
        return fail("valid Graph drive item was parsed incorrectly");
    }
    item["size"] = -1;
    if (!throws_with(
            [&] {
                static_cast<void>(detail::parse_drive_item(
                    item,
                    "drive item",
                    detail::ContentValidation::relaxed,
                    detail::DriveItemKind::file_or_directory
                ));
            },
            "invalid drive item metadata"
        ) ||
        !throws_with(
            [] {
                static_cast<void>(detail::parse_drive_item(
                    Json::object(),
                    "drive item",
                    detail::ContentValidation::strict,
                    detail::DriveItemKind::file_or_directory
                ));
            },
            "missing required drive item data"
        )) {
        return fail("invalid Graph drive item was accepted");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (!throws_with<onedrive::graph::AccountAuthenticationError>(
            [] {
                detail::require_successful_graph_response(
                    Json{{"error", {{"message", "account token rejected"}}}},
                    401,
                    "delta"
                );
            },
            "account token rejected"
        )) {
        return fail("Graph authentication failure lost its account-wide type");
    }
    if (test_unsupported_formats() != EXIT_SUCCESS ||
        test_unsupported_ancestry() != EXIT_SUCCESS ||
        test_ancestor_metadata_validation() != EXIT_SUCCESS ||
        test_ancestor_failure_and_cancellation_boundaries() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (const int result = test_repeated_page_fetcher();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_paths_and_errors(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_status_and_retry_helpers();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_json_and_facets(); result != EXIT_SUCCESS) {
        return result;
    }
    return test_hashes_and_drive_items();
}
