#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/sync/blocked_policy.hpp"

#include <algorithm>
#include "upload_cases.hpp"

namespace onedrive::test::sync::upload_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_directory_upload_journal_failure() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto journal_failure_root =
        temporary.path() / "directory-journal-failure";
    std::filesystem::create_directories(journal_failure_root / "Pending");
    FakeItemStore journal_failure_items;
    journal_failure_items.saved_delta_link = "saved";
    journal_failure_items.fail_pending_upload_save = true;
    FakeGraphClient journal_failure_graph;
    FakeMetrics journal_failure_metrics;
    auto journal_failure_config = config_for(journal_failure_root, false);
    journal_failure_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            journal_failure_config,
            journal_failure_graph,
            journal_failure_items,
            journal_failure_metrics,
            &default_console
        }
                              .synchronize());
        return fail("directory journal failure was accepted");
    } catch (const std::runtime_error&) {
    }
    if (journal_failure_graph.directory_create_count != 0 ||
        !journal_failure_items.pending_uploads_by_path.empty()) {
        return fail("unjournaled directory creation reached Microsoft Graph");
    }
    journal_failure_items.fail_pending_upload_save = false;
    if (onedrive::sync::SyncEngine{
            journal_failure_config,
            journal_failure_graph,
            journal_failure_items,
            journal_failure_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        journal_failure_graph.created_directory_paths !=
            std::vector<std::string>{"Pending"} ||
        !journal_failure_items.pending_uploads_by_path.empty()) {
        return fail("directory creation did not recover after journal failure");
    }
    return EXIT_SUCCESS;
}

int test_nested_directory_uploads() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto root = temporary.path() / "directory-uploads";
    std::filesystem::create_directories(root / "Empty");
    std::filesystem::create_directories(root / "Parent" / "Child");
    {
        std::ofstream output{root / "Parent" / "Child" / "file.txt"};
        output << "payload";
    }
    FakeItemStore items;
    items.saved_delta_link = "saved";
    FakeGraphClient graph;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::success ||
        graph.created_directory_paths !=
            std::vector<std::string>{
                "Empty",
                "Parent",
                "Parent/Child",
            } ||
        graph.uploaded_paths !=
            std::vector<std::string>{"Parent/Child/file.txt"} ||
        graph.upload_count != 1 || graph.directory_create_count != 3 ||
        items.size() != 4 || !items.find("me", "directory-1") ||
        items.find("me", "directory-1")->local_device == 0 ||
        items.find("me", "directory-1")->local_inode == 0 ||
        !items.pending_uploads_by_path.empty() || !metrics.last_success) {
        return fail(
            "nested local directories were not created before their files"
        );
    }

    for (const auto* id : {"directory-1", "directory-2", "directory-3"}) {
        const auto directory = items.find("me", id);
        if (!directory || !directory->directory || directory->content_hash) {
            return fail(
                "uploaded directory state should not have a content hash"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_directory_upload_quota_recovery() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto resource_root = temporary.path() / "directory-resource";
    std::filesystem::create_directories(resource_root / "Continued");
    std::filesystem::create_directories(resource_root / "Quota");
    FakeItemStore resource_items;
    resource_items.saved_delta_link = "saved";
    FakeGraphClient resource_graph;
    resource_graph.directory_resource_error_path = "Quota";
    FakeMetrics resource_metrics;
    auto resource_config = config_for(resource_root, false);
    resource_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    static_cast<void>(onedrive::sync::SyncEngine{
        resource_config,
        resource_graph,
        resource_items,
        resource_metrics,
        &default_console
    }
                          .synchronize());
    const auto resource_pending = resource_items.pending_uploads("me");
    if (resource_pending.size() != 1 ||
        resource_pending[0].remote_path != "Quota" ||
        resource_pending[0].failure_code != "remote_quota" ||
        resource_pending[0].failure_attempt_count != 1 ||
        !resource_pending[0].directory ||
        !resource_items.find("me", "directory-1")) {
        return fail(
            "directory quota failure was not persisted while uploads continued"
        );
    }
    resource_graph.directory_resource_error_path.clear();
    static_cast<void>(onedrive::sync::SyncEngine{
        resource_config,
        resource_graph,
        resource_items,
        resource_metrics,
        &default_console
    }
                          .synchronize());
    if (!resource_items.pending_uploads("me").empty() ||
        !resource_items.find("me", "directory-3")) {
        return fail("directory quota failure did not recover");
    }
    return EXIT_SUCCESS;
}

int test_directory_upload_conflict() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto conflict_root = temporary.path() / "directory-conflict";
    std::filesystem::create_directories(conflict_root / "Existing");
    FakeItemStore conflict_items;
    conflict_items.saved_delta_link = "saved";
    FakeGraphClient conflict_graph;
    conflict_graph.directory_conflict = true;
    FakeMetrics conflict_metrics;
    auto conflict_config = config_for(conflict_root, false);
    conflict_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            conflict_config,
            conflict_graph,
            conflict_items,
            conflict_metrics,
            &default_console
        }
                              .synchronize());
        return fail("remote directory conflict was accepted");
    } catch (const std::runtime_error&) {
    }
    if (!conflict_items.pending_uploads_by_path.empty() ||
        conflict_metrics.last_success) {
        return fail("definite directory conflict retained recovery state");
    }
    return EXIT_SUCCESS;
}

int test_directory_upload_commit_recovery() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto recovery_root = temporary.path() / "directory-recovery";
    std::filesystem::create_directories(recovery_root / "Recover");
    FakeItemStore recovery_items;
    recovery_items.saved_delta_link = "saved";
    recovery_items.fail_commit_upload = true;
    FakeGraphClient recovery_graph;
    FakeMetrics recovery_metrics;
    auto recovery_config = config_for(recovery_root, false);
    recovery_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            recovery_config,
            recovery_graph,
            recovery_items,
            recovery_metrics,
            &default_console
        }
                              .synchronize());
        return fail("directory commit failure was not reported");
    } catch (const std::runtime_error&) {
    }
    if (recovery_items.pending_uploads_by_path.size() != 1 ||
        !recovery_items.pending_uploads_by_path.begin()->second.directory) {
        return fail("directory commit failure did not retain its journal");
    }
    recovery_items.fail_commit_upload = false;
    recovery_graph.directory_conflict = true;
    recovery_graph.lookup_item = onedrive::graph::RemoteItem{
        .id = "recovered-directory",
        .name = "Recover",
        .etag = "recovered-etag",
        .parent_id = "root-id",
        .remote_path = "Recover",
        .directory = true,
    };
    if (onedrive::sync::SyncEngine{
            recovery_config,
            recovery_graph,
            recovery_items,
            recovery_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        recovery_graph.directory_create_count != 2 ||
        !recovery_items.pending_uploads_by_path.empty() ||
        !recovery_items.find("me", "recovered-directory") ||
        recovery_items.find("me", "recovered-directory")->content_hash ||
        !recovery_metrics.last_success) {
        return fail("pending directory creation was not recovered");
    }
    return EXIT_SUCCESS;
}

int test_directory_upload_selective_boundary() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto selective_root = temporary.path() / "selective-directory-upload";
    std::filesystem::create_directories(selective_root / "Included");
    std::filesystem::create_directories(selective_root / "Excluded");
    const auto sync_list = temporary.path() / "directory-sync-list";
    {
        std::ofstream output{sync_list};
        output << "/Included/\n";
    }
    FakeItemStore selective_items;
    selective_items.saved_delta_link = "saved";
    FakeGraphClient selective_graph;
    FakeMetrics selective_metrics;
    auto selective_config = config_for(selective_root, false);
    selective_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    selective_config.sync_list = sync_list;
    if (onedrive::sync::SyncEngine{
            selective_config,
            selective_graph,
            selective_items,
            selective_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        selective_graph.created_directory_paths !=
            std::vector<std::string>{"Included"} ||
        !std::filesystem::is_directory(selective_root / "Excluded")) {
        return fail("selective sync uploaded an excluded local directory");
    }
    return EXIT_SUCCESS;
}

int test_directory_upload_type_conflict() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto type_root = temporary.path() / "directory-type-conflict";
    std::filesystem::create_directories(type_root / "Tracked");
    FakeItemStore type_items;
    type_items.saved_delta_link = "saved";
    type_items.items.emplace(
        "tracked-file", tracked_item(type_root, "tracked-file", "Tracked")
    );
    FakeGraphClient type_graph;
    FakeMetrics type_metrics;
    auto type_config = config_for(type_root, false);
    type_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    static_cast<void>(onedrive::sync::SyncEngine{
        type_config, type_graph, type_items, type_metrics, &default_console
    }
                          .synchronize());
    if (type_graph.directory_create_count != 0 || type_items.size() != 1) {
        return fail("local directory replaced a tracked remote file");
    }
    return EXIT_SUCCESS;
}

int test_directory_changed_during_upload() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto changing_root = temporary.path() / "changing-directory-upload";
    const auto changing_directory = changing_root / "Changing";
    std::filesystem::create_directories(changing_directory);
    FakeItemStore changing_items;
    changing_items.saved_delta_link = "saved";
    FakeGraphClient changing_graph;
    changing_graph.before_directory_return = [&] {
        std::filesystem::remove(changing_directory);
    };
    FakeMetrics changing_metrics;
    auto changing_config = config_for(changing_root, false);
    changing_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            changing_config,
            changing_graph,
            changing_items,
            changing_metrics,
            &default_console
        }
                              .synchronize());
        return fail("removed local directory was committed after creation");
    } catch (const std::runtime_error&) {
    }
    if (changing_items.pending_uploads_by_path.size() != 1) {
        return fail("changed local directory did not retain recovery state");
    }
    return EXIT_SUCCESS;
}

int test_directory_upload_recovery_conflict() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto recovery_conflict_root =
        temporary.path() / "directory-recovery-conflict";
    const auto recovery_conflict_directory =
        recovery_conflict_root / "Conflict";
    std::filesystem::create_directories(recovery_conflict_directory);
    FakeItemStore recovery_conflict_items;
    recovery_conflict_items.saved_delta_link = "saved";
    recovery_conflict_items.pending_uploads_by_path.emplace(
        "Conflict",
        onedrive::storage::PendingUpload{
            .drive_id = "me",
            .remote_path = "Conflict",
            .local_path = recovery_conflict_directory,
            .directory = true,
        }
    );
    FakeGraphClient recovery_conflict_graph;
    recovery_conflict_graph.directory_conflict = true;
    recovery_conflict_graph.lookup_item = file("remote-file", "Conflict", 0);
    recovery_conflict_graph.changes = {
        recovery_conflict_graph.lookup_item.value(),
    };
    FakeMetrics recovery_conflict_metrics;
    auto recovery_conflict_config = config_for(recovery_conflict_root, false);
    recovery_conflict_config.sync_mode =
        onedrive::sync::SyncMode::bidirectional;
    if (onedrive::sync::SyncEngine{
            recovery_conflict_config,
            recovery_conflict_graph,
            recovery_conflict_items,
            recovery_conflict_metrics,
            &default_console
        }
                .synchronize() != exit_code::blocked ||
        !recovery_conflict_items.pending_uploads_by_path.empty() ||
        recovery_conflict_items.applied_delta.blocked_upserts.size() != 1 ||
        recovery_conflict_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_modification" ||
        !recovery_conflict_metrics.last_success) {
        return fail(
            "directory recovery conflict did not defer to remote delta"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::upload_cases
