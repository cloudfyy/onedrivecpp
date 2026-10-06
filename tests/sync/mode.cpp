#include "support.hpp"

namespace {

using namespace onedrive::test::sync;

int test_upload_only() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "upload-only";
    std::filesystem::create_directories(root);
    {
        std::ofstream output{root / "new.txt"};
        output << "new";
    }
    {
        std::ofstream output{root / "preserved.txt"};
        output << "data";
    }
    {
        std::ofstream output{root / "missing.txt"};
        output << "data";
    }
    {
        std::ofstream output{root / "modified.txt"};
        output << "data";
    }

    FakeItemStore items;
    items.saved_delta_link = "saved";
    items.items.emplace(
        "preserved", tracked_item(root, "preserved", "preserved.txt")
    );
    items.items.emplace(
        "missing", tracked_item(root, "missing", "missing.txt")
    );
    items.items.emplace(
        "modified", tracked_item(root, "modified", "modified.txt")
    );
    std::filesystem::remove(root / "missing.txt");
    {
        std::ofstream output{root / "modified.txt"};
        output << "local-change";
    }
    items.pending_deletes_by_id.emplace(
        "missing",
        onedrive::storage::PendingDelete{
            .drive_id = "me",
            .remote_id = "missing",
            .expected_etag = "etag",
            .remote_path = "missing.txt",
            .local_path = root / "missing.txt",
            .directory = false,
        }
    );

    FakeGraphClient graph;
    graph.changes = {
        file("remote-only", "remote-only.txt", 6),
        file("modified", "modified.txt", 6),
        deleted_item("preserved"),
    };
    graph.contents["remote-only"] = "remote";
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::upload_only;
    config.delete_policy = onedrive::sync::DeletePolicy::preserve;

    std::ostringstream output;
    std::ostringstream error;
    const onedrive::cli::Console console{
        {
            .color = onedrive::cli::ColorMode::never,
            .output = onedrive::cli::OutputMode::json,
        },
        output,
        error
    };
    const auto result =
        onedrive::sync::SyncEngine{config, graph, items, metrics, &console}
            .synchronize();
    std::ranges::sort(graph.uploaded_paths);

    if (result != 0 || graph.download_count != 0 ||
        !graph.deleted_items.empty() ||
        graph.uploaded_paths !=
            std::vector<std::string>{"modified.txt", "new.txt"} ||
        !std::filesystem::exists(root / "preserved.txt") ||
        std::filesystem::exists(root / "remote-only.txt") ||
        !items.pending_deletes_by_id.contains("missing") ||
        !items.find("me", "missing") || !metrics.last_success ||
        !output.str().contains("\"download_files\":\"0\"") ||
        !output.str().contains("\"local_removals\":\"0\"")) {
        return fail(
            "upload-only mode changed local content or deleted remote content"
        );
    }
    return EXIT_SUCCESS;
}

int test_download_only_propagates_remote_deletions() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "download-only-propagate";
    std::filesystem::create_directories(root);
    {
        std::ofstream output{root / "deleted.txt"};
        output << "data";
    }
    {
        std::ofstream output{root / "missing.txt"};
        output << "data";
    }
    {
        std::ofstream output{root / "local-only.txt"};
        output << "local";
    }

    FakeItemStore items;
    items.saved_delta_link = "saved";
    items.items.emplace(
        "deleted", tracked_item(root, "deleted", "deleted.txt")
    );
    items.items.emplace(
        "missing", tracked_item(root, "missing", "missing.txt")
    );
    std::filesystem::remove(root / "missing.txt");
    items.pending_deletes_by_id.emplace(
        "missing",
        onedrive::storage::PendingDelete{
            .drive_id = "me",
            .remote_id = "missing",
            .expected_etag = "etag",
            .remote_path = "missing.txt",
            .local_path = root / "missing.txt",
            .directory = false,
        }
    );

    FakeGraphClient graph;
    graph.changes = {
        deleted_item("deleted"),
        file("remote-only", "remote-only.txt", 6),
    };
    graph.contents["remote-only"] = "remote";
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.delete_policy = onedrive::sync::DeletePolicy::propagate;

    if (onedrive::sync::SyncEngine{config, graph, items, metrics}.synchronize(
        ) != 0 ||
        std::filesystem::exists(root / "deleted.txt") ||
        onedrive::test::read_file(root / "remote-only.txt") != "remote" ||
        graph.upload_count != 0 || !graph.deleted_items.empty() ||
        !items.pending_deletes_by_id.contains("missing") ||
        !items.find("me", "missing") || items.find("me", "deleted") ||
        !metrics.last_success) {
        return fail(
            "download-only mode did not safely propagate remote deletions"
        );
    }
    return EXIT_SUCCESS;
}

int test_download_only_preserves_and_untracks_local_items() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "download-only-preserve";
    std::filesystem::create_directories(root);
    {
        std::ofstream output{root / "modified.txt"};
        output << "data";
    }

    FakeItemStore items;
    items.saved_delta_link = "saved";
    items.items.emplace(
        "modified", tracked_item(root, "modified", "modified.txt")
    );
    {
        std::ofstream output{root / "modified.txt"};
        output << "local-change";
    }

    FakeGraphClient graph;
    graph.changes = {deleted_item("modified")};
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.delete_policy = onedrive::sync::DeletePolicy::propagate;
    if (onedrive::sync::SyncEngine{config, graph, items, metrics}.synchronize(
        ) != 2 ||
        !std::filesystem::exists(root / "modified.txt") ||
        !items.find("me", "modified") || items.blocked.size() != 1 ||
        items.blocked.front().reason_code != "local_modification") {
        return fail(
            "download-only remote deletion did not preserve a local conflict"
        );
    }

    graph.changes.clear();
    config.delete_policy = onedrive::sync::DeletePolicy::preserve;
    if (onedrive::sync::SyncEngine{config, graph, items, metrics}.synchronize(
        ) != 0 ||
        onedrive::test::read_file(root / "modified.txt") != "local-change" ||
        items.find("me", "modified") || !items.blocked.empty() ||
        graph.upload_count != 0 || !graph.deleted_items.empty() ||
        items.applied_delta.removals != std::vector<std::string>{"modified"} ||
        items.applied_delta.blocked_removals !=
            std::vector<std::string>{"modified"} ||
        !metrics.last_success) {
        return fail(
            "download-only preserve did not resolve the remote tombstone"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (const auto result = test_upload_only(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = test_download_only_propagates_remote_deletions();
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_download_only_preserves_and_untracks_local_items();
}
