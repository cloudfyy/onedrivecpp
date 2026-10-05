#include "sync/core/plan.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <filesystem>

#include <stdexcept>
#include <string>

namespace {

using onedrive::test::fail;

onedrive::graph::RemoteItem item(
    std::string id,
    std::string path,
    bool directory = false
) {
    return {
        .id = std::move(id),
        .name = std::filesystem::path{path}.filename().string(),
        .etag = "etag",
        .parent_id = "parent",
        .remote_path = std::move(path),
        .last_modified = "2026-10-02T00:00:00Z",
        .size = directory ? 0 : 4,
        .directory = directory,
    };
}

}  // namespace

int main() {
    namespace detail = onedrive::sync::detail;

    auto root = item("root", "root", true);
    root.root = true;
    auto removed = item("removed", "removed.txt");
    removed.deleted = true;
    auto plan = detail::SyncPlan::build(
        {
            .changes = {
                root,
                item("deep", "a/b", true),
                item("shallow", "a", true),
                item("file", "a/file.txt"),
                removed,
            },
            .delta_link = "https://graph.example.test/delta",
        },
        "me",
        "/sync",
        true,
        ""
    );
    if (plan.change_count() != 5 || plan.directory_count() != 2 ||
        plan.download_count() != 1 || plan.removal_count() != 1 ||
        plan.download_bytes() != 4 ||
        plan.directory(0).id != "shallow" ||
        plan.directory(1).id != "deep" ||
        plan.download(0).id != "file") {
        return fail("delta result was not converted into the expected plan");
    }
    if (plan.removal(0).id != "removed") {
        return fail("remote deletion was not retained for local execution");
    }
    auto& state = plan.state_for("file");
    if (state.drive_id != "me" || state.local_path != "/sync/a/file.txt") {
        return fail("planned item state was incorrect");
    }
    plan.complete_removal("removed");
    const auto delta = plan.release_state_delta();
    if (!delta.replace_drive_items || delta.upserts.size() != 3 ||
        delta.removals != std::vector<std::string>{"removed"} ||
        delta.delta_link != "https://graph.example.test/delta") {
        return fail("planned persistent delta was incorrect");
    }

    auto move_plan = detail::SyncPlan::build(
        {
            .changes = {item("moved", "new/name.txt")},
            .delta_link = "https://graph.example.test/move",
        },
        "me",
        "/sync",
        false,
        "",
        {},
        {
            {
                .drive_id = "me",
                .remote_id = "moved",
                .name = "name.txt",
                .remote_path = "old/name.txt",
                .local_path = "/sync/old/name.txt",
            },
        }
    );
    if (move_plan.move_count() != 1 ||
        move_plan.move(0).id != "moved") {
        return fail("remote path change was not planned as a local move");
    }

    auto blocked_removal_plan = detail::SyncPlan::build(
        {
            .changes = {removed},
            .delta_link = "https://graph.example.test/blocked-removal",
        },
        "me",
        "/sync",
        false,
        ""
    );
    blocked_removal_plan.block_removal(
        {
            .drive_id = "me",
            .remote_id = "removed",
            .name = "removed.txt",
            .remote_path = "removed.txt",
            .local_path = "/sync/removed.txt",
            .size = 4,
        },
        "local_modification",
        "local file changed"
    );
    const auto blocked_removal_delta =
        blocked_removal_plan.release_state_delta();
    if (!blocked_removal_delta.removals.empty() ||
        blocked_removal_delta.blocked_upserts.size() != 1 ||
        !blocked_removal_delta.blocked_upserts[0].deleted ||
        blocked_removal_delta.blocked_upserts[0].reason_code !=
            "local_modification") {
        return fail("blocked remote deletion lost its retry metadata");
    }

    auto malware = item("malware", "blocked.exe");
    malware.malware = true;
    auto malware_plan = detail::SyncPlan::build(
        {
            .changes = {malware},
            .delta_link = "https://graph.example.test/malware",
        },
        "me",
        "/sync",
        false,
        ""
    );
    if (malware_plan.download_count() != 0 ||
        malware_plan.download_bytes() != 0 ||
        malware_plan.blocked_count() != 1 ||
        malware_plan.blocked(0).reason_code != "malware_detected") {
        return fail("Graph malware item was not blocked before download");
    }

    auto invalid = item("invalid", "invalid.txt");
    invalid.size = -1;
    try {
        static_cast<void>(detail::SyncPlan::build(
            {
                .changes = {invalid},
                .delta_link = "https://graph.example.test/invalid",
            },
            "me",
            "/sync",
            false,
            ""
        ));
        return fail("negative remote file size was accepted");
    } catch (const std::runtime_error&) {
    }
    return EXIT_SUCCESS;
}
