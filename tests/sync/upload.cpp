#include "support.hpp"

#include <algorithm>

namespace {

using namespace onedrive::test::sync;

int test_local_file_uploads() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto dry_root = temporary.path() / "dry-uploads";
    std::filesystem::create_directories(dry_root);
    {
        std::ofstream output{dry_root / "new.txt"};
        output << "new";
    }
    FakeItemStore dry_items;
    dry_items.saved_delta_link = "saved";
    FakeGraphClient dry_graph;
    FakeMetrics dry_metrics;
    auto dry_config = config_for(dry_root, true);
    dry_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    if (onedrive::sync::SyncEngine{
            dry_config, dry_graph, dry_items, dry_metrics, &default_console
        }
                .synchronize() != 0 ||
        dry_graph.upload_count != 0 || dry_items.apply_count != 0) {
        return fail("upload dry run changed remote or local state");
    }

    const auto root = temporary.path() / "uploads";
    std::filesystem::create_directories(root);
    {
        std::ofstream output{root / "new.txt"};
        output << "new";
    }
    {
        std::ofstream output{root / "modified.txt"};
        output << "data";
    }
    {
        std::ofstream output{root / "unchanged.txt"};
        output << "data";
    }
    {
        std::ofstream output{root / "tracked-directory"};
        output << "conflict";
    }
    {
        std::ofstream output{root / "report.safeBackup-20261004-0001.txt"};
        output << "backup";
    }
    {
        std::ofstream output{root / ".new.txt.onedrive-move-recovery"};
        output << "staged";
    }
    std::filesystem::create_symlink("new.txt", root / "linked.txt");

    FakeItemStore items;
    items.saved_delta_link = "saved";
    items.items.emplace(
        "modified", tracked_item(root, "modified", "modified.txt")
    );
    items.items.emplace(
        "unchanged", tracked_item(root, "unchanged", "unchanged.txt")
    );
    items.items.emplace(
        "tracked-directory",
        tracked_item(root, "tracked-directory", "tracked-directory", true)
    );
    {
        std::ofstream output{root / "modified.txt"};
        output << "changed";
    }

    FakeGraphClient graph;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    const auto result =
        onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
            .synchronize();
    std::ranges::sort(graph.uploaded_paths);
    bool upload_snapshot_found = false;
    for (const auto& entry : std::filesystem::directory_iterator{root}) {
        if (entry.path().filename().string().contains(".onedrive-upload-")) {
            upload_snapshot_found = true;
        }
    }
    const auto modified = items.find("me", "modified");
    const auto unchanged = items.find("me", "unchanged");
    if (result != 2 || graph.upload_count != 2 ||
        graph.uploaded_paths !=
            std::vector<std::string>{"modified.txt", "new.txt"} ||
        !modified ||
        (modified->etag != "uploaded-etag-1" &&
         modified->etag != "uploaded-etag-2") ||
        modified->local_size != 7 || modified->local_device == 0 ||
        modified->local_inode == 0 || !unchanged ||
        unchanged->local_device == 0 || unchanged->local_inode == 0 ||
        (!items.find("me", "uploaded-1") && !items.find("me", "uploaded-2")) ||
        upload_snapshot_found || !metrics.last_success) {
        return fail("new and modified local files were not uploaded safely");
    }

    const auto concurrent_root = temporary.path() / "concurrent-uploads";
    std::filesystem::create_directories(concurrent_root);
    for (int index = 0; index < 4; ++index) {
        std::ofstream{
            concurrent_root / ("file-" + std::to_string(index) + ".txt")
        } << "parallel";
    }
    FakeItemStore concurrent_items;
    concurrent_items.saved_delta_link = "saved";
    FakeGraphClient concurrent_graph;
    concurrent_graph.upload_delay = std::chrono::milliseconds{50};
    FakeMetrics concurrent_metrics;
    auto concurrent_config = config_for(concurrent_root, false);
    concurrent_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    concurrent_config.upload_concurrency = 2;
    const auto concurrent_result =
        onedrive::sync::SyncEngine{
            concurrent_config,
            concurrent_graph,
            concurrent_items,
            concurrent_metrics,
            &default_console
        }
            .synchronize();
    if (concurrent_result != 0 || concurrent_graph.upload_count != 4 ||
        concurrent_graph.maximum_concurrent_uploads != 2 ||
        concurrent_items.size() != 4 || !concurrent_metrics.last_success) {
        return fail(
            std::format(
                "uploads did not respect the configured concurrency: "
                "result={}, count={}, maximum={}, items={}, success={}",
                concurrent_result,
                concurrent_graph.upload_count,
                concurrent_graph.maximum_concurrent_uploads.load(),
                concurrent_items.size(),
                concurrent_metrics.last_success
            )
        );
    }

    const auto cancelled_root =
        temporary.path() / "cancelled-concurrent-uploads";
    std::filesystem::create_directories(cancelled_root);
    for (int index = 0; index < 6; ++index) {
        std::ofstream{
            cancelled_root / ("file-" + std::to_string(index) + ".txt")
        } << "cancel";
    }
    FakeItemStore cancelled_items;
    cancelled_items.saved_delta_link = "saved";
    FakeGraphClient cancelled_graph;
    cancelled_graph.upload_delay = std::chrono::milliseconds{30};
    cancelled_graph.fatal_upload_error = true;
    FakeMetrics cancelled_metrics;
    auto cancelled_config = config_for(cancelled_root, false);
    cancelled_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    cancelled_config.upload_concurrency = 2;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            cancelled_config,
            cancelled_graph,
            cancelled_items,
            cancelled_metrics,
            &default_console
        }
                              .synchronize());
        return fail("fatal parallel upload failure was ignored");
    } catch (const std::runtime_error& error) {
        if (!std::string_view{error.what()}.contains(
                "simulated fatal upload failure"
            )) {
            return fail("parallel upload replaced the fatal error");
        }
    }
    if (cancelled_graph.upload_count > 2 || cancelled_metrics.last_success) {
        return fail(
            "parallel uploads continued taking work after a fatal failure"
        );
    }

    const auto resource_root = temporary.path() / "upload-resources";
    std::filesystem::create_directories(resource_root);
    {
        std::ofstream output{resource_root / "quota.txt"};
        output << "quota";
    }
    {
        std::ofstream output{resource_root / "continued.txt"};
        output << "continued";
    }
    FakeItemStore resource_items;
    resource_items.saved_delta_link = "saved";
    FakeGraphClient resource_graph;
    resource_graph.upload_resource_error_path = "quota.txt";
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
    const auto quota_pending = resource_items.pending_uploads("me");
    if (quota_pending.size() != 1 ||
        quota_pending[0].remote_path != "quota.txt" ||
        quota_pending[0].failure_code != "remote_quota" ||
        quota_pending[0].failure_attempt_count != 1 ||
        !resource_items.find("me", "uploaded-1")) {
        return fail(
            "remote quota failure was not persisted while uploads continued"
        );
    }
    static_cast<void>(onedrive::sync::SyncEngine{
        resource_config,
        resource_graph,
        resource_items,
        resource_metrics,
        &default_console
    }
                          .synchronize());
    if (resource_items.pending_uploads("me").size() != 1 ||
        resource_items.pending_uploads("me")[0].failure_attempt_count != 2) {
        return fail("remote quota retry did not update its failure state");
    }
    resource_graph.upload_resource_error_path.clear();
    static_cast<void>(onedrive::sync::SyncEngine{
        resource_config,
        resource_graph,
        resource_items,
        resource_metrics,
        &default_console
    }
                          .synchronize());
    if (!resource_items.pending_uploads("me").empty() ||
        !resource_items.find("me", "uploaded-4")) {
        return fail("remote quota upload did not recover");
    }

    const auto storage_root = temporary.path() / "upload-storage";
    std::filesystem::create_directories(storage_root);
    {
        std::ofstream output{storage_root / "blocked.txt"};
        output << "blocked";
    }
    {
        std::ofstream output{storage_root / "continued.txt"};
        output << "continued";
    }
    std::vector<std::filesystem::path> occupied_snapshots;
    for (std::size_t attempt = 1; attempt <= 100; ++attempt) {
        const auto collision = storage_root / (".blocked.txt.onedrive-upload-" +
                                               std::to_string(::getpid()) +
                                               "-" + std::to_string(attempt));
        std::ofstream output{collision};
        output << "occupied";
        occupied_snapshots.push_back(collision);
    }
    FakeItemStore storage_items;
    storage_items.saved_delta_link = "saved";
    FakeGraphClient storage_graph;
    FakeMetrics storage_metrics;
    auto storage_config = config_for(storage_root, false);
    storage_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    static_cast<void>(onedrive::sync::SyncEngine{
        storage_config,
        storage_graph,
        storage_items,
        storage_metrics,
        &default_console
    }
                          .synchronize());
    const auto storage_pending = storage_items.pending_uploads("me");
    if (storage_pending.size() != 1 ||
        storage_pending[0].remote_path != "blocked.txt" ||
        storage_pending[0].failure_code != "local_storage" ||
        storage_pending[0].failure_attempt_count != 1 ||
        storage_graph.uploaded_paths !=
            std::vector<std::string>{"continued.txt"}) {
        return fail(
            "local snapshot failure was not persisted while uploads continued"
        );
    }
    static_cast<void>(onedrive::sync::SyncEngine{
        storage_config,
        storage_graph,
        storage_items,
        storage_metrics,
        &default_console
    }
                          .synchronize());
    const auto retried_storage_pending = storage_items.pending_uploads("me");
    if (retried_storage_pending.size() != 1 ||
        retried_storage_pending[0].failure_attempt_count != 2) {
        return fail("local snapshot retry did not update its failure state");
    }
    for (const auto& collision : occupied_snapshots) {
        std::filesystem::remove(collision);
    }
    static_cast<void>(onedrive::sync::SyncEngine{
        storage_config,
        storage_graph,
        storage_items,
        storage_metrics,
        &default_console
    }
                          .synchronize());
    if (!storage_items.pending_uploads("me").empty() ||
        storage_graph.uploaded_paths != std::vector<std::string>{
                                            "continued.txt",
                                            "blocked.txt",
                                        }) {
        return fail("local snapshot resource failure did not recover");
    }

    if (::geteuid() != 0) {
        const auto permission_root = temporary.path() / "upload-permission";
        std::filesystem::create_directories(permission_root);
        const auto unreadable = permission_root / "unreadable.txt";
        {
            std::ofstream output{unreadable};
            output << "unreadable";
        }
        std::filesystem::permissions(unreadable, std::filesystem::perms::none);
        FakeItemStore permission_items;
        permission_items.saved_delta_link = "saved";
        FakeGraphClient permission_graph;
        FakeMetrics permission_metrics;
        auto permission_config = config_for(permission_root, false);
        permission_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
        static_cast<void>(onedrive::sync::SyncEngine{
            permission_config,
            permission_graph,
            permission_items,
            permission_metrics,
            &default_console
        }
                              .synchronize());
        const auto permission_pending = permission_items.pending_uploads("me");
        if (permission_pending.size() != 1 ||
            permission_pending[0].failure_code != "local_permission") {
            return fail("local upload permission failure was not persisted");
        }
        std::filesystem::permissions(
            unreadable,
            std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write
        );
        static_cast<void>(onedrive::sync::SyncEngine{
            permission_config,
            permission_graph,
            permission_items,
            permission_metrics,
            &default_console
        }
                              .synchronize());
        if (!permission_items.pending_uploads("me").empty() ||
            permission_graph.upload_count != 1) {
            return fail("local upload permission failure did not recover");
        }
    }

    const auto removed_root = temporary.path() / "removed-upload-resource";
    std::filesystem::create_directories(removed_root);
    FakeItemStore removed_items;
    removed_items.saved_delta_link = "saved";
    removed_items.save_pending_upload({
        .drive_id = "me",
        .remote_path = "removed.txt",
        .local_path = removed_root / "removed.txt",
        .failure_code = "local_read",
        .failure_message = "file was unreadable",
        .failure_attempt_count = 1,
    });
    FakeGraphClient removed_graph;
    FakeMetrics removed_metrics;
    auto removed_config = config_for(removed_root, false);
    removed_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    static_cast<void>(onedrive::sync::SyncEngine{
        removed_config,
        removed_graph,
        removed_items,
        removed_metrics,
        &default_console
    }
                          .synchronize());
    if (!removed_items.pending_uploads("me").empty()) {
        return fail("removed local upload retained its resource failure");
    }
    return EXIT_SUCCESS;
}

int test_local_directory_uploads() {
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
                .synchronize() != 0 ||
        journal_failure_graph.created_directory_paths !=
            std::vector<std::string>{"Pending"} ||
        !journal_failure_items.pending_uploads_by_path.empty()) {
        return fail("directory creation did not recover after journal failure");
    }

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
                .synchronize() != 0 ||
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
            return fail("uploaded directory state should not have a content hash");
        }
    }

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
                .synchronize() != 0 ||
        recovery_graph.directory_create_count != 2 ||
        !recovery_items.pending_uploads_by_path.empty() ||
        !recovery_items.find("me", "recovered-directory") ||
        recovery_items.find("me", "recovered-directory")->content_hash ||
        !recovery_metrics.last_success) {
        return fail("pending directory creation was not recovered");
    }

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
                .synchronize() != 0 ||
        selective_graph.created_directory_paths !=
            std::vector<std::string>{"Included"} ||
        !std::filesystem::is_directory(selective_root / "Excluded")) {
        return fail("selective sync uploaded an excluded local directory");
    }

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
    recovery_conflict_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    if (onedrive::sync::SyncEngine{
            recovery_conflict_config,
            recovery_conflict_graph,
            recovery_conflict_items,
            recovery_conflict_metrics,
            &default_console
        }
                .synchronize() != 2 ||
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

int test_local_change_during_upload() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto root = temporary.path() / "changing-upload";
    std::filesystem::create_directories(root);
    const auto local = root / "changing.txt";
    {
        std::ofstream output{local};
        output << "old";
    }
    FakeItemStore items;
    items.saved_delta_link = "saved";
    FakeGraphClient graph;
    graph.before_upload_return = [&] {
        std::ofstream output{local, std::ios::trunc};
        output << "new contents";
    };
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != 0 ||
        graph.upload_count != 1) {
        return fail("initial changing local file upload failed");
    }
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != 0 ||
        graph.upload_count != 2) {
        return fail("local change during upload was marked as synchronized");
    }
    const auto uploaded = items.find("me", "uploaded-1");
    if (!uploaded || uploaded->local_size != 12 ||
        !items.pending_uploads_by_path.empty() || !metrics.last_success) {
        return fail("follow-up upload did not commit the changed local file");
    }
    return EXIT_SUCCESS;
}

int test_pending_upload_recovery() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto root = temporary.path() / "pending-upload";
    std::filesystem::create_directories(root);
    const auto local = root / "recover.txt";
    {
        std::ofstream output{local};
        output << "payload";
    }
    FakeItemStore items;
    items.saved_delta_link = "saved";
    items.fail_commit_upload = true;
    FakeGraphClient graph;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                              .synchronize());
        return fail("upload commit failure was not reported");
    } catch (const std::runtime_error&) {
    }
    if (items.pending_uploads_by_path.size() != 1 ||
        !std::filesystem::exists(
            items.pending_uploads_by_path.begin()->second.snapshot_path
        ) ||
        metrics.last_success) {
        return fail("failed upload commit did not preserve its journal");
    }

    items.fail_commit_upload = false;
    graph.upload_conflict = true;
    graph.lookup_item = file("recovered-upload", "recover.txt", 7);
    graph.contents.emplace("recovered-upload", "payload");
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != 0 ||
        graph.upload_count != 2 || graph.download_count != 1 ||
        !items.pending_uploads_by_path.empty() ||
        !items.find("me", "recovered-upload") ||
        std::ranges::any_of(
            std::filesystem::directory_iterator{root},
            [](const auto& entry) {
                return entry.path().filename().string().contains(
                    ".onedrive-upload-"
                );
            }
        ) ||
        !metrics.last_success) {
        return fail("completed pending upload was not verified and recovered");
    }
    return EXIT_SUCCESS;
}

int test_upload_checkpoint_recovery() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto root = temporary.path() / "upload-checkpoint";
    std::filesystem::create_directories(root);
    {
        std::ofstream output{root / "large.bin", std::ios::binary};
        output << "payload";
    }
    FakeItemStore items;
    items.saved_delta_link = "saved";
    FakeGraphClient graph;
    graph.upload_checkpoint = onedrive::graph::UploadSession{
        .upload_url = "https://upload.example.test/session?secret=1",
        .expiration = "2099-10-05T09:00:00Z",
        .completed_bytes = 4,
    };
    graph.fail_after_upload_checkpoint = true;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                              .synchronize());
        return fail("interrupted upload session was not reported");
    } catch (const std::runtime_error&) {
    }
    if (items.pending_uploads_by_path.size() != 1 ||
        items.pending_uploads_by_path.begin()->second.completed_bytes != 4 ||
        items.pending_uploads_by_path.begin()->second.upload_url !=
            "https://upload.example.test/session?secret=1") {
        return fail("upload checkpoint was not persisted before interruption");
    }

    graph.fail_after_upload_checkpoint = false;
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != 0 ||
        graph.upload_count != 2 || graph.upload_sessions.size() != 2 ||
        !graph.upload_sessions[1] ||
        graph.upload_sessions[1]->completed_bytes != 4 ||
        graph.upload_sessions[1]->upload_url !=
            "https://upload.example.test/session?secret=1" ||
        !items.pending_uploads_by_path.empty() || !metrics.last_success) {
        return fail("persisted upload checkpoint was not resumed");
    }

    const auto failing_root = temporary.path() / "upload-checkpoint-failure";
    std::filesystem::create_directories(failing_root);
    {
        std::ofstream output{failing_root / "large.bin", std::ios::binary};
        output << "payload";
    }
    FakeItemStore failing_items;
    failing_items.saved_delta_link = "saved";
    failing_items.fail_upload_checkpoint_save = true;
    FakeGraphClient failing_graph;
    failing_graph.upload_checkpoint = onedrive::graph::UploadSession{
        .upload_url = "https://upload.example.test/uncommitted",
        .expiration = "2099-10-05T09:00:00Z",
        .completed_bytes = 4,
    };
    FakeMetrics failing_metrics;
    auto failing_config = config_for(failing_root, false);
    failing_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            failing_config,
            failing_graph,
            failing_items,
            failing_metrics,
            &default_console
        }
                              .synchronize());
        return fail("upload checkpoint persistence failure was ignored");
    } catch (const std::runtime_error& error) {
        if (!std::string_view{error.what()}.contains(
                "checkpoint persistence failure"
            )) {
            return fail("upload checkpoint persistence error was replaced");
        }
    }
    if (failing_graph.upload_count != 1 ||
        failing_items.pending_uploads_by_path.size() != 1 ||
        !failing_items.pending_uploads_by_path.begin()->second.upload_url.empty(
        )) {
        return fail("failed upload checkpoint was treated as committed");
    }
    return EXIT_SUCCESS;
}

int test_pending_upload_recovery_conflict() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
    const auto prepare_conflict = [](const std::filesystem::path& root,
                                     FakeItemStore& items) {
        std::filesystem::create_directories(root);
        const auto local = root / "conflict.txt";
        const auto snapshot = root / ".conflict.txt.onedrive-upload-crash";
        {
            std::ofstream output{local};
            output << "payload";
        }
        std::filesystem::copy_file(local, snapshot);
        auto previous = tracked_item(root, "remote-conflict", "conflict.txt");
        previous.local_size = 4;
        items.saved_delta_link = "saved";
        items.items.emplace("remote-conflict", previous);
        items.pending_uploads_by_path.emplace(
            "conflict.txt",
            onedrive::storage::PendingUpload{
                .drive_id = "me",
                .remote_path = "conflict.txt",
                .local_path = local,
                .snapshot_path = snapshot,
                .content_fingerprint = "239f59ed55e737c77147cf55ad0c1b030b6d7ee"
                                       "748a7426952f9b852d5a935e5",
                .local_size = 7,
                .local_modified_ticks = previous.local_modified_ticks,
                .remote_id = std::optional<std::string>{"remote-conflict"},
                .expected_etag = previous.etag,
            }
        );
        return snapshot;
    };
    const auto prepare_graph = [](FakeGraphClient& graph) {
        graph.upload_conflict = true;
        graph.lookup_item = file("remote-conflict", "conflict.txt", 7);
        graph.changes = {graph.lookup_item.value()};
        graph.contents.emplace("remote-conflict", "changed");
    };

    const auto block_root = temporary.path() / "pending-upload-conflict-block";
    FakeItemStore block_items;
    const auto block_snapshot = prepare_conflict(block_root, block_items);
    FakeGraphClient block_graph;
    prepare_graph(block_graph);
    FakeMetrics block_metrics;
    auto block_config = config_for(block_root, false);
    block_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    if (onedrive::sync::SyncEngine{
            block_config,
            block_graph,
            block_items,
            block_metrics,
            &default_console
        }
                .synchronize() != 2 ||
        !block_items.pending_uploads_by_path.empty() ||
        std::filesystem::exists(block_snapshot) ||
        block_items.applied_delta.blocked_upserts.size() != 1 ||
        block_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_modification" ||
        block_graph.download_count != 1 || !block_metrics.last_success) {
        return fail("upload recovery conflict did not defer to block policy");
    }
    {
        std::ifstream input{block_root / "conflict.txt"};
        std::string content{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}
        };
        if (content != "payload") {
            return fail("block policy replaced conflicting local content");
        }
    }

    const auto backup_root =
        temporary.path() / "pending-upload-conflict-backup";
    FakeItemStore backup_items;
    const auto backup_snapshot = prepare_conflict(backup_root, backup_items);
    FakeGraphClient backup_graph;
    prepare_graph(backup_graph);
    FakeMetrics backup_metrics;
    auto backup_config = config_for(backup_root, false);
    backup_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    backup_config.local_conflict =
        onedrive::config::LocalConflictPolicy::backup;
    if (onedrive::sync::SyncEngine{
            backup_config,
            backup_graph,
            backup_items,
            backup_metrics,
            &default_console
        }
                .synchronize() != 0 ||
        !backup_items.pending_uploads_by_path.empty() ||
        std::filesystem::exists(backup_snapshot) ||
        backup_graph.download_count != 2 ||
        !backup_items.applied_delta.blocked_upserts.empty() ||
        !backup_metrics.last_success) {
        return fail("upload recovery conflict did not defer to backup policy");
    }
    std::filesystem::path preserved;
    for (const auto& entry : std::filesystem::directory_iterator{backup_root}) {
        if (entry.path().filename().string().starts_with(
                "conflict.safeBackup-"
            )) {
            preserved = entry.path();
        }
    }
    std::ifstream remote_input{backup_root / "conflict.txt"};
    const std::string remote_content{
        std::istreambuf_iterator<char>{remote_input},
        std::istreambuf_iterator<char>{}
    };
    std::ifstream local_input{preserved};
    const std::string local_content{
        std::istreambuf_iterator<char>{local_input},
        std::istreambuf_iterator<char>{}
    };
    if (preserved.empty() || remote_content != "changed" ||
        local_content != "payload") {
        return fail("backup policy did not preserve both conflict versions");
    }

    const auto size_root = temporary.path() / "pending-upload-conflict-size";
    FakeItemStore size_items;
    const auto size_snapshot = prepare_conflict(size_root, size_items);
    FakeGraphClient size_graph;
    size_graph.upload_conflict = true;
    size_graph.lookup_item = file("remote-conflict", "conflict.txt", 8);
    size_graph.changes = {size_graph.lookup_item.value()};
    size_graph.contents.emplace("remote-conflict", "remote!!");
    FakeMetrics size_metrics;
    auto size_config = config_for(size_root, false);
    size_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    if (onedrive::sync::SyncEngine{
            size_config, size_graph, size_items, size_metrics, &default_console
        }
                .synchronize() != 2 ||
        !size_items.pending_uploads_by_path.empty() ||
        std::filesystem::exists(size_snapshot) ||
        size_graph.download_count != 0 ||
        size_items.applied_delta.blocked_upserts.size() != 1 ||
        !size_metrics.last_success) {
        return fail("different-size upload conflict was not reconciled safely");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (const int result = test_local_file_uploads(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_local_directory_uploads();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_local_change_during_upload();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_pending_upload_recovery();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_upload_checkpoint_recovery();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_pending_upload_recovery_conflict();
        result != EXIT_SUCCESS) {
        return result;
    }
    return EXIT_SUCCESS;
}
