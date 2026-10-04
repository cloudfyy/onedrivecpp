#include "selective_sync.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

onedrive::graph::RemoteItem item(
    std::string id,
    std::string path,
    bool directory = false
) {
    const auto separator = path.rfind('/');
    return {
        .id = std::move(id),
        .name = separator == std::string::npos ?
            path :
            path.substr(separator + 1),
        .etag = "etag",
        .remote_path = std::move(path),
        .size = directory ? 0 : 4,
        .directory = directory,
    };
}

void write_rules(
    const std::filesystem::path& path,
    const std::string& contents
) {
    std::ofstream output{path};
    output << contents;
    if (!output) {
        throw std::runtime_error("cannot write selective sync test rules");
    }
}

}  // namespace

int main() {
    namespace detail = onedrive::sync::detail;
    onedrive::test::TemporaryDirectory temporary_directory;
    const auto rules_path = temporary_directory.path() / "sync_list";
    write_rules(
        rules_path,
        "# selective sync rules\n"
        "/Documents/\n"
        "!/Documents/Private/*\n"
        "/Projects/**/README.*\n"
        "Pictures/*.jpg\n"
    );
    const auto rules = detail::SyncList::load(rules_path);
    if (rules.rule_count() != 4 ||
        !rules.includes("Documents", true) ||
        rules.includes("Documents", false) ||
        !rules.includes("Documents/report.txt", false) ||
        rules.includes("Documents/Private/secret.txt", false) ||
        !rules.includes("Projects/README.md", false) ||
        !rules.includes("Projects/a/b/README.txt", false) ||
        !rules.includes("Archive/Pictures/photo.jpg", false) ||
        rules.includes("Archive/Pictures/photo.png", false) ||
        rules.includes("Archive/Documents/report.txt", false)) {
        return fail("sync list matching semantics were incorrect");
    }

    const auto fingerprint = rules.fingerprint();
    write_rules(
        rules_path,
        "\n# comments and blank lines do not affect the rules\n"
        "/Documents/\n"
        "!/Documents/Private/*\n"
        "/Projects/**/README.*\n"
        "Pictures/*.jpg\n"
    );
    if (detail::SyncList::load(rules_path).fingerprint() != fingerprint) {
        return fail("non-semantic sync list comments changed its fingerprint");
    }

    write_rules(rules_path, "");
    const auto empty_rules = detail::SyncList::load(rules_path);
    if (empty_rules.includes("anything.txt", false)) {
        return fail("an empty sync list did not exclude everything");
    }

    write_rules(rules_path, "/Parent/Child/file.txt\n");
    const auto ancestor_rules = detail::SyncList::load(rules_path);
    auto root = item("root", "", true);
    root.root = true;
    auto removed = item("removed", "");
    removed.deleted = true;
    auto filtered = detail::filter_delta(
        {
            .changes = {
                root,
                item("parent", "Parent", true),
                item("child", "Parent/Child", true),
                item("selected", "Parent/Child/file.txt"),
                item("excluded", "Parent/other.txt"),
                removed,
            },
            .delta_link = "delta-1",
        },
        ancestor_rules,
        [](std::string_view) {
            return false;
        },
        true
    );
    std::vector<std::string> filtered_ids;
    for (const auto& change : filtered.delta.changes) {
        filtered_ids.push_back(change.id);
    }
    if (filtered_ids != std::vector<std::string>{
            "root", "parent", "child", "selected", "removed"
        } ||
        filtered.excluded != 1 ||
        filtered.delta.delta_link != "delta-1") {
        return fail("full delta filtering did not retain required ancestors");
    }

    const std::unordered_set<std::string> tracked{"tracked"};
    filtered = detail::filter_delta(
        {
            .changes = {
                item("tracked", "Other/tracked.txt"),
                item("untracked", "Other/untracked.txt"),
            },
            .delta_link = "delta-2",
        },
        ancestor_rules,
        [&](std::string_view id) {
            return tracked.contains(std::string{id});
        },
        false
    );
    if (filtered.delta.changes.size() != 1 ||
        filtered.delta.changes.front().id != "tracked" ||
        !filtered.delta.changes.front().deleted ||
        filtered.excluded != 2) {
        return fail("incremental filtering did not remove excluded snapshots");
    }

    write_rules(
        rules_path,
        "!/Parent/\n"
        "/Parent/Child/file.txt\n"
    );
    const auto excluded_parent_rules = detail::SyncList::load(rules_path);
    filtered = detail::filter_delta(
        {
            .changes = {
                item("parent", "Parent", true),
                item("child", "Parent/Child", true),
                item("selected", "Parent/Child/file.txt"),
            },
            .delta_link = "delta-3",
        },
        excluded_parent_rules,
        [](std::string_view) {
            return false;
        },
        true
    );
    if (!filtered.delta.changes.empty() || filtered.excluded != 3) {
        return fail("an excluded parent did not override child inclusions");
    }

    write_rules(rules_path, "/invalid/**suffix\n");
    try {
        static_cast<void>(detail::SyncList::load(rules_path));
        return fail("an invalid recursive wildcard was accepted");
    } catch (const std::runtime_error&) {
    }
    {
        std::ofstream output{rules_path, std::ios::binary};
        output << "/Documents/";
        output.put(static_cast<char>(0xc0));
        output.put(static_cast<char>(0xaf));
        output << '\n';
    }
    try {
        static_cast<void>(detail::SyncList::load(rules_path));
        return fail("invalid UTF-8 sync list content was accepted");
    } catch (const std::runtime_error&) {
    }
    try {
        static_cast<void>(
            detail::SyncList::load(
                temporary_directory.path() / "missing-sync-list"
            )
        );
        return fail("a missing sync list was accepted");
    } catch (const std::runtime_error&) {
    }

    return EXIT_SUCCESS;
}
