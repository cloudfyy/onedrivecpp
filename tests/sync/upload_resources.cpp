#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/sync/blocked_policy.hpp"

#include <algorithm>
#include "upload_cases.hpp"

namespace onedrive::test::sync::upload_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_upload_quota_recovery() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
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
    if (quota_pending.size() != 1 || resource_items.blocked.size() != 1 ||
        onedrive::sync::blocked_item_strategy(
            resource_items.blocked[0].reason_code
        )
                .traits()
                .origin != onedrive::sync::BlockedOrigin::local ||
        resource_items.blocked[0].remote_path != "quota.txt" ||
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
        resource_items.blocked.size() != 1 ||
        resource_items.blocked[0].attempt_count != 2 ||
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
        !resource_items.blocked.empty() ||
        !resource_items.find("me", "uploaded-4")) {
        return fail("remote quota upload did not recover");
    }
    return EXIT_SUCCESS;
}

int test_upload_snapshot_recovery() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
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
    return EXIT_SUCCESS;
}

int test_upload_permission_recovery() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
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
    return EXIT_SUCCESS;
}

int test_removed_upload_resource() {
    const onedrive::cli::Console default_console;
    onedrive::test::TemporaryDirectory temporary;
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

} // namespace onedrive::test::sync::upload_cases
