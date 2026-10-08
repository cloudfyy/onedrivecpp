#include "graph/support.hpp"
#include "support/common.hpp"

#include <chrono>
#include <cstdlib>
#include <limits>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace detail = onedrive::graph::client_detail;
using detail::Json;
using onedrive::test::fail;

using onedrive::test::throws_with;

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
            "invalid sha256Hash"
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
            "invalid sha256Hash"
        )) {
        return fail("invalid Graph file hash facet was accepted");
    }
    const auto sha = detail::item_content_hash(
        Json{{
            "file",
            {{"hashes", {{"sha256Hash", sha256}}}},
        }}
    );
    const auto quick = detail::item_content_hash(
        Json{{
            "file",
            {{"hashes", {{"quickXorHash", quick_xor}}}},
        }}
    );
    if (!sha || sha->value != sha256 || !quick || quick->value != quick_xor) {
        return fail("valid Graph file hash was not parsed");
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
        {"file", {{"hashes", {{"sha256Hash", sha256}}}}},
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
