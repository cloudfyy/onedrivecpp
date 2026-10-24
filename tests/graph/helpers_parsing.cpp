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
#include "helpers_cases.hpp"

namespace onedrive::test::graph::helper_cases {

namespace detail = onedrive::graph::client_detail;
using detail::Json;
using onedrive::test::fail;

using onedrive::test::throws_with;

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

} // namespace onedrive::test::graph::helper_cases
