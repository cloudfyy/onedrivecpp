#include "sync/filter/remote_path.hpp"
#include "sync/filter/selective.hpp"
#include "support/common.hpp"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using onedrive::storage::DeltaApplyMode;
using onedrive::test::fail;

onedrive::graph::RemoteItem item(
    std::string id,
    std::string path,
    bool directory = false,
    std::int64_t size = 4
) {
    const auto separator = path.rfind('/');
    return {
        .id = std::move(id),
        .name =
            separator == std::string::npos ? path : path.substr(separator + 1),
        .etag = "etag",
        .remote_path = std::move(path),
        .size = directory ? 0 : size,
        .directory = directory,
    };
}

void write_rules(
    const std::filesystem::path& path, const std::string& contents
) {
    std::ofstream output{path};
    output << contents;
    if (!output) {
        throw std::runtime_error("cannot write selective sync test rules");
    }
}

} // namespace

int main() {
    namespace detail = onedrive::sync::detail;
    constexpr auto file = detail::SyncItemKind::file;
    constexpr auto directory = detail::SyncItemKind::directory;
    if (!detail::remote_path_is_descendant("Folder/File", "Folder") ||
        !detail::remote_path_is_descendant("Folder/Nested/File", "Folder") ||
        detail::remote_path_is_descendant("Folder", "Folder") ||
        detail::remote_path_is_descendant("Folder2/File", "Folder") ||
        detail::remote_path_is_descendant("", "") ||
        detail::remote_path_is_descendant("Folder", "")) {
        return onedrive::test::fail(
            "remote path descendant boundaries were incorrect"
        );
    }
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
    if (rules.rule_count() != 4 || !rules.includes("Documents", directory) ||
        rules.includes("Documents", file) ||
        !rules.includes("Documents/report.txt", file) ||
        rules.includes("Documents/Private/secret.txt", file) ||
        !rules.includes("Projects/README.md", file) ||
        !rules.includes("Projects/a/b/README.txt", file) ||
        !rules.includes("Archive/Pictures/photo.jpg", file) ||
        rules.includes("Archive/Pictures/photo.png", file) ||
        rules.includes("Archive/Documents/report.txt", file)) {
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
    if (empty_rules.includes("anything.txt", file)) {
        return fail("an empty sync list did not exclude everything");
    }
    const auto root_file_rules = detail::SyncList::load(rules_path, true);
    if (!root_file_rules.includes("root.txt", file) ||
        root_file_rules.includes("Folder/nested.txt", file) ||
        root_file_rules.includes("RootFolder", directory) ||
        root_file_rules.fingerprint() == empty_rules.fingerprint()) {
        return fail("implicit root-file selection semantics were incorrect");
    }
    write_rules(rules_path, "!/blocked.txt\n");
    const auto excluded_root_file_rules =
        detail::SyncList::load(rules_path, true);
    if (excluded_root_file_rules.includes("blocked.txt", file) ||
        !excluded_root_file_rules.includes("included.txt", file)) {
        return fail("root-file exclusion did not override implicit inclusion");
    }

    const auto policy_root = temporary_directory.path() / "policy-root";
    std::filesystem::create_directories(policy_root / "Ignored");
    std::filesystem::create_directories(policy_root / "Visible");
    {
        std::ofstream marker{policy_root / "Ignored" / ".nosync"};
    }
    const auto policy = detail::SyncList::configured({
        .sync_root = policy_root,
        .nosync_enabled = true,
        .dotfiles = onedrive::config::DotfilePolicy::exclude,
        .maximum_file_size_bytes = 4,
    });
    if (policy.includes("Ignored", directory) ||
        policy.includes("Ignored/file.txt", file, 4) ||
        policy.includes("Ignored/.nosync", file, 0) ||
        policy.includes(".hidden", directory) ||
        policy.includes("Visible/.secret", file, 1) ||
        policy.includes("Visible/large.bin", file, 5) ||
        !policy.includes("Visible/small.bin", file, 4)) {
        return fail(".nosync, dotfile, or maximum-size policy was incorrect");
    }
    const auto policy_fingerprint = policy.fingerprint();
    std::filesystem::remove(policy_root / "Ignored" / ".nosync");
    const auto marker_removed = detail::SyncList::configured({
        .sync_root = policy_root,
        .nosync_enabled = true,
        .dotfiles = onedrive::config::DotfilePolicy::exclude,
        .maximum_file_size_bytes = 4,
    });
    if (!marker_removed.includes("Ignored/file.txt", file, 4) ||
        marker_removed.fingerprint() == policy_fingerprint) {
        return fail(".nosync marker changes did not update filter state");
    }
    const auto policies_disabled = detail::SyncList::configured({
        .sync_root = policy_root,
        .nosync_enabled = false,
    });
    const auto default_policy = detail::SyncList::configured({
        .sync_root = policy_root / "missing",
    });
    if (!default_policy.fingerprint().empty() ||
        !policies_disabled.includes("Ignored/.nosync", file, 1) ||
        !policies_disabled.includes(".hidden/file.txt", file, 100)) {
        return fail("disabled synchronization policies still excluded paths");
    }
    write_rules(rules_path, "/Visible/\n");
    if (detail::SyncList::configured({
            .rules_path = rules_path,
            .sync_root = policy_root,
        })
            .fingerprint() != detail::SyncList::load(rules_path).fingerprint()) {
        return fail("default policies changed the legacy sync-list fingerprint");
    }
    {
        std::ofstream marker{policy_root / ".nosync"};
    }
    const auto root_marker = detail::SyncList::configured({
        .sync_root = policy_root,
    });
    if (root_marker.includes("Visible/file.txt", file, 1) ||
        root_marker.fingerprint().empty()) {
        return fail("root .nosync marker did not exclude all descendants");
    }
    std::filesystem::remove(policy_root / ".nosync");
    std::filesystem::create_symlink(
        policy_root / "missing-marker", policy_root / "Visible" / ".nosync"
    );
    const auto symlink_marker = detail::SyncList::configured({
        .sync_root = policy_root,
        .maximum_file_size_bytes = 4,
    });
    if (!symlink_marker.includes("Visible/unknown.bin", file) ||
        !symlink_marker.includes("Visible/small.bin", file, 4)) {
        return fail("symlink marker or unknown size excluded eligible files");
    }

    auto policy_filtered = detail::filter_delta(
        {
            .changes =
                {
                    item("small", "Visible/small.bin"),
                    item("large", "Visible/large.bin", false, 5),
                    item("hidden", ".hidden"),
                },
            .delta_link = "policy-delta",
        },
        policy,
        [](std::string_view) { return false; },
        DeltaApplyMode::replace
    );
    if (policy_filtered.delta.changes.size() != 1 ||
        policy_filtered.delta.changes[0].id != "small" ||
        policy_filtered.excluded != 2) {
        return fail("policy delta filtering retained excluded files");
    }
    auto move_only_filtered = detail::filter_delta(
        {
            .changes = {item("tracked-large", "Visible/large.bin", false, 5)},
            .delta_link = "move-only-predicate",
        },
        policy,
        [tracked = std::make_unique<std::string>("tracked-large")](
            std::string_view remote_id
        ) { return remote_id == *tracked; },
        DeltaApplyMode::merge
    );
    if (move_only_filtered.snapshot_removals !=
            std::vector<std::string>{"tracked-large"} ||
        move_only_filtered.retained_remote_ids !=
            std::vector<std::string>{"tracked-large"}) {
        return fail("move-only tracked-item predicate was not supported");
    }

    auto root_files_filtered = detail::filter_delta(
        {
            .changes =
                {
                    item("root-file", "root.txt"),
                    item("nested-file", "Folder/nested.txt"),
                    item("root-directory", "Folder", true),
                },
            .delta_link = "root-files-delta",
        },
        excluded_root_file_rules,
        [](std::string_view) { return false; },
        DeltaApplyMode::replace
    );
    if (root_files_filtered.delta.changes.size() != 1 ||
        root_files_filtered.delta.changes[0].id != "root-file" ||
        root_files_filtered.excluded != 2) {
        return fail("root-file delta filtering retained unexpected items");
    }

    write_rules(rules_path, "/Parent/Child/file.txt\n");
    const auto ancestor_rules = detail::SyncList::load(rules_path);
    auto root = item("root", "", true);
    root.root = true;
    auto removed = item("removed", "");
    removed.deleted = true;
    auto filtered = detail::filter_delta(
        {
            .changes =
                {
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
        [](std::string_view) { return false; },
        DeltaApplyMode::replace
    );
    std::vector<std::string> filtered_ids;
    for (const auto& change : filtered.delta.changes) {
        filtered_ids.push_back(change.id);
    }
    if (filtered_ids !=
            std::vector<std::string>{
                "root", "parent", "child", "selected", "removed"
            } ||
        filtered.excluded != 1 || filtered.delta.delta_link != "delta-1") {
        return fail("full delta filtering did not retain required ancestors");
    }

    const std::unordered_set<std::string> tracked{"tracked"};
    filtered = detail::filter_delta(
        {
            .changes =
                {
                    item("tracked", "Other/tracked.txt"),
                    item("untracked", "Other/untracked.txt"),
                },
            .delta_link = "delta-2",
        },
        ancestor_rules,
        [&](std::string_view id) { return tracked.contains(std::string{id}); },
        DeltaApplyMode::merge
    );
    if (!filtered.delta.changes.empty() ||
        filtered.snapshot_removals != std::vector<std::string>{"tracked"} ||
        filtered.retained_remote_ids != std::vector<std::string>{"tracked"} ||
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
            .changes =
                {
                    item("parent", "Parent", true),
                    item("child", "Parent/Child", true),
                    item("selected", "Parent/Child/file.txt"),
                },
            .delta_link = "delta-3",
        },
        excluded_parent_rules,
        [](std::string_view) { return false; },
        DeltaApplyMode::replace
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
        static_cast<void>(detail::SyncList::load(
            temporary_directory.path() / "missing-sync-list"
        ));
        return fail("a missing sync list was accepted");
    } catch (const std::runtime_error&) {
    }

    return EXIT_SUCCESS;
}
