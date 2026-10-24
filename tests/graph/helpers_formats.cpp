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

} // namespace onedrive::test::graph::helper_cases
