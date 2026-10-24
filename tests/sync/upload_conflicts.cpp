#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/sync/blocked_policy.hpp"

#include <algorithm>
#include "upload_cases.hpp"

namespace onedrive::test::sync::upload_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

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
        graph.lookup_item->etag = "remote-updated-etag";
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
                .synchronize() != exit_code::blocked ||
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
                .synchronize() != exit_code::success ||
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
    size_graph.lookup_item->etag = "remote-updated-etag";
    size_graph.changes = {size_graph.lookup_item.value()};
    size_graph.contents.emplace("remote-conflict", "remote!!");
    FakeMetrics size_metrics;
    auto size_config = config_for(size_root, false);
    size_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    if (onedrive::sync::SyncEngine{
            size_config, size_graph, size_items, size_metrics, &default_console
        }
                .synchronize() != exit_code::blocked ||
        !size_items.pending_uploads_by_path.empty() ||
        std::filesystem::exists(size_snapshot) ||
        size_graph.download_count != 0 ||
        size_items.applied_delta.blocked_upserts.size() != 1 ||
        !size_metrics.last_success) {
        return fail("different-size upload conflict was not reconciled safely");
    }

    const auto unchanged_root =
        temporary.path() / "pending-upload-unchanged-echo";
    FakeItemStore unchanged_items;
    const auto unchanged_snapshot =
        prepare_conflict(unchanged_root, unchanged_items);
    FakeGraphClient unchanged_graph;
    unchanged_graph.upload_conflict = true;
    unchanged_graph.lookup_item = file("remote-conflict", "conflict.txt", 4);
    unchanged_graph.delta_override = [&](const auto&) {
        unchanged_graph.upload_conflict = false;
        return onedrive::graph::DeltaResult{
            .changes = {unchanged_graph.lookup_item.value()},
            .delta_link = "next",
        };
    };
    FakeMetrics unchanged_metrics;
    auto unchanged_config = config_for(unchanged_root, false);
    unchanged_config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    if (onedrive::sync::SyncEngine{
            unchanged_config,
            unchanged_graph,
            unchanged_items,
            unchanged_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        !unchanged_items.pending_uploads_by_path.empty() ||
        !unchanged_items.blocked.empty() ||
        std::filesystem::exists(unchanged_snapshot) ||
        unchanged_graph.download_count != 0 ||
        unchanged_graph.upload_count != 2 ||
        onedrive::test::read_file(unchanged_root / "conflict.txt") !=
            "payload" ||
        !unchanged_metrics.last_success) {
        return fail("unchanged recovery Delta echo blocked the local upload");
    }
    const auto updated = unchanged_items.find("me", "remote-conflict");
    if (!updated || updated->local_size != 7 ||
        updated->etag != "uploaded-etag-2") {
        return fail(
            "reconciled local upload did not replace its trusted snapshot"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::upload_cases
