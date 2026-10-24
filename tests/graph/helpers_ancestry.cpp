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

} // namespace onedrive::test::graph::helper_cases
