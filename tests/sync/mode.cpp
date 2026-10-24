#include "support/progress.hpp"
#include "support.hpp"
#include "onedrive/exit_code.hpp"

#include <algorithm>

namespace {

namespace exit_code = onedrive::exit_code;

using namespace onedrive::test::sync;

int test_missing_file_delta_echo() {
    TemporaryDirectory temporary;
    const onedrive::cli::Console console;
    for (const bool download_only : {false, true}) {
        for (const int version : {0, 1, 2, 3}) {
            const bool remote_changed = version == 1 || version == 3;
            const auto root =
                temporary.path() /
                (std::to_string(download_only) + std::to_string(version));
            std::filesystem::create_directories(root);
            std::ofstream{root / "file.txt"} << "data";
            FakeItemStore items;
            items.saved_delta_link = "saved";
            items.items.emplace("file", tracked_item(root, "file", "file.txt"));
            if (version >= 2) {
                items.items.at("file").ctag = "content-v1";
            }
            std::filesystem::remove(root / "file.txt");
            FakeGraphClient graph;
            auto remote = file("file", "file.txt", 4);
            if (version != 0) {
                remote.etag = "new-etag";
            }
            if (version >= 2) {
                remote.ctag = remote_changed ? "content-v2" : "content-v1";
            }
            graph.changes = {remote};
            graph.contents["file"] = "data";
            FakeMetrics metrics;
            auto config = config_for(root, false);
            config.sync_mode = download_only
                                   ? onedrive::sync::SyncMode::download_only
                                   : onedrive::sync::SyncMode::bidirectional;
            const bool restores = download_only || remote_changed;
            if (onedrive::sync::SyncEngine{
                    config, graph, items, metrics, &console
                }
                        .synchronize() != exit_code::success ||
                graph.download_count != (restores ? 1 : 0) ||
                std::filesystem::exists(root / "file.txt") != restores ||
                graph.deleted_items.size() != (restores ? 0U : 1U)) {
                return fail(
                    "missing-file Delta echo changed download or conflict "
                    "semantics"
                );
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_modified_file_delta_echo(bool retry_blocked) {
    TemporaryDirectory temporary;
    const onedrive::cli::Console console{{.quiet = true}};
    for (const bool download_only : {false, true}) {
        for (const bool backup : {false, true}) {
            for (const int version : {0, 1, 2, 3}) {
                for (const bool same_size : {false, true}) {
                    const bool remote_changed = version == 1 || version == 3;
                    const auto label =
                        std::to_string(download_only) + std::to_string(backup) +
                        std::to_string(version) + std::to_string(same_size);
                    const auto root = temporary.path() / label;
                    std::filesystem::create_directories(root);
                    const auto destination = root / "file.txt";
                    onedrive::test::write_file(destination, "data");
                    auto previous = tracked_item(root, "file", "file.txt");
                    if (version >= 2) {
                        previous.ctag = "content-v1";
                    }
                    FakeItemStore items;
                    items.saved_delta_link = "saved";
                    items.items.emplace("file", previous);
                    const std::string edited =
                        same_size ? "EDIT" : "local-edit";
                    const auto edited_time =
                        std::filesystem::last_write_time(destination) +
                        std::chrono::seconds{1};
                    onedrive::test::write_file(destination, edited);
                    std::filesystem::last_write_time(destination, edited_time);
                    auto remote = file("file", "file.txt", 4);
                    if (version != 0) {
                        remote.etag = "new-etag";
                    }
                    if (version >= 2) {
                        remote.ctag =
                            remote_changed ? "content-v2" : "content-v1";
                    }
                    FakeGraphClient graph;
                    if (retry_blocked) {
                        items.blocked.push_back({
                            .drive_id = "me",
                            .remote_id = remote.id,
                            .parent_id = remote.parent_id,
                            .name = remote.name,
                            .etag = remote.etag,
                            .ctag = remote.ctag,
                            .remote_path = remote.remote_path,
                            .last_modified = remote.last_modified,
                            .size = remote.size,
                            .reason_code = "local_modification",
                            .reason_message = "previous conflict",
                            .attempt_count = 1,
                        });
                    } else {
                        graph.changes = {remote};
                    }
                    const std::string remote_contents =
                        remote_changed ? "next" : "data";
                    graph.contents["file"] = remote_contents;
                    std::string uploaded;
                    graph.during_upload = [&] {
                        if (items.pending_uploads_by_path.size() != 1) {
                            throw std::runtime_error{
                                "expected one journaled upload snapshot"
                            };
                        }
                        uploaded = onedrive::test::read_file(
                            items.pending_uploads_by_path.begin()
                                ->second.snapshot_path
                        );
                    };
                    FakeMetrics metrics;
                    auto config = config_for(root, false);
                    config.sync_mode =
                        download_only ? onedrive::sync::SyncMode::download_only
                                      : onedrive::sync::SyncMode::bidirectional;
                    config.local_conflict =
                        backup ? onedrive::config::LocalConflictPolicy::backup
                               : onedrive::config::LocalConflictPolicy::block;
                    const auto synchronize = [&] {
                        return onedrive::sync::SyncEngine{
                            config, graph, items, metrics, &console
                        }
                            .synchronize();
                    };
                    const bool local_update = !download_only && !remote_changed;
                    const bool downloads = !local_update && backup;
                    const bool blocked = !local_update && !backup;
                    const auto result = synchronize();
                    std::filesystem::path preserved;
                    unsigned backups = 0;
                    for (const auto& entry :
                         std::filesystem::directory_iterator{root}) {
                        if (entry.path().filename().string().starts_with(
                                "file.safeBackup-"
                            )) {
                            preserved = entry.path();
                            ++backups;
                        }
                    }
                    if (result != (blocked ? exit_code::blocked
                                           : exit_code::success) ||
                        graph.download_count != (downloads ? 1 : 0) ||
                        graph.upload_count != (local_update ? 1 : 0) ||
                        !graph.deleted_items.empty() ||
                        items.blocked.size() != (blocked ? 1U : 0U) ||
                        (blocked && items.blocked.front().reason_code !=
                                        "local_modification") ||
                        backups != (downloads ? 1U : 0U) ||
                        (downloads &&
                         onedrive::test::read_file(preserved) != edited) ||
                        (local_update && uploaded != edited) ||
                        onedrive::test::read_file(destination) !=
                            (downloads ? remote_contents : edited) ||
                        !items.pending.empty() ||
                        !items.pending_uploads_by_path.empty()) {
                        return fail(
                            "modified-file Delta echo changed upload or "
                            "conflict semantics: " +
                            label + ", retry " + std::to_string(retry_blocked)
                        );
                    }
                    if (!local_update) {
                        continue;
                    }
                    const auto saved = std::ranges::find(
                        items.applied_delta.upserts,
                        "file",
                        &onedrive::storage::ItemState::remote_id
                    );
                    if (saved == items.applied_delta.upserts.end() ||
                        saved->local_size != previous.local_size ||
                        saved->local_modified_ticks !=
                            previous.local_modified_ticks ||
                        saved->etag != remote.etag ||
                        saved->ctag != remote.ctag ||
                        graph.uploaded_paths !=
                            std::vector<std::string>{"file.txt"}) {
                        return fail(
                            "unchanged Delta echo lost the trusted local "
                            "baseline or current remote metadata"
                        );
                    }
                    graph.changes.clear();
                    const auto updated = items.find("me", "file");
                    if (!updated || updated->etag != "uploaded-etag-1" ||
                        updated->local_size !=
                            static_cast<std::int64_t>(edited.size()) ||
                        updated->local_modified_ticks !=
                            std::chrono::duration_cast<
                                std::chrono::nanoseconds>(
                                edited_time.time_since_epoch()
                            )
                                .count()) {
                        return fail(
                            "local upload did not commit its edited snapshot"
                        );
                    }
                    if (synchronize() != exit_code::success ||
                        graph.upload_count != 1 || graph.download_count != 0 ||
                        !items.blocked.empty() ||
                        onedrive::test::read_file(destination) != edited) {
                        return fail(
                            "unchanged Delta echo repeated or lost a local "
                            "upload"
                        );
                    }
                }
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_unchanged_echo_preserves_path_conflicts() {
    const onedrive::cli::Console console{{.quiet = true}};
    for (const bool symbolic_link : {false, true}) {
        for (const bool backup : {false, true}) {
            TemporaryDirectory temporary;
            const auto root = temporary.path() / "sync";
            std::filesystem::create_directory(root);
            const auto destination = root / "file.txt";
            const auto outside = temporary.path() / "outside.txt";
            onedrive::test::write_file(destination, "data");
            onedrive::test::write_file(outside, "outside");
            FakeItemStore items;
            items.saved_delta_link = "saved";
            items.items.emplace("file", tracked_item(root, "file", "file.txt"));
            std::filesystem::remove(destination);
            if (symbolic_link) {
                std::filesystem::create_symlink(outside, destination);
            } else {
                std::filesystem::create_directory(destination);
            }
            FakeGraphClient graph;
            graph.changes = {file("file", "file.txt", 4)};
            graph.contents["file"] = "data";
            FakeMetrics metrics;
            auto config = config_for(root, false);
            config.sync_mode = onedrive::sync::SyncMode::bidirectional;
            config.local_conflict =
                backup ? onedrive::config::LocalConflictPolicy::backup
                       : onedrive::config::LocalConflictPolicy::block;
            if (onedrive::sync::SyncEngine{
                    config, graph, items, metrics, &console
                }
                        .synchronize() != exit_code::blocked ||
                graph.download_count != 0 || graph.upload_count != 0 ||
                !graph.deleted_items.empty() ||
                onedrive::test::read_file(outside) != "outside" ||
                (symbolic_link ? !std::filesystem::is_symlink(destination)
                               : !std::filesystem::is_directory(destination)) ||
                std::ranges::none_of(items.blocked, [&](const auto& item) {
                    return item.remote_id == "file" &&
                           item.reason_code == (!symbolic_link && !backup
                                                    ? "local_modification"
                                                    : "local_path_conflict");
                })) {
                return fail(
                    "unchanged Delta echo bypassed local path protection"
                );
            }
        }
    }
    return EXIT_SUCCESS;
}

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

    if (result != exit_code::success || graph.download_count != 0 ||
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
    const onedrive::cli::Console default_console;
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

    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::success ||
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
    const onedrive::cli::Console default_console;
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
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::blocked ||
        !std::filesystem::exists(root / "modified.txt") ||
        !items.find("me", "modified") || items.blocked.size() != 1 ||
        items.blocked.front().reason_code != "local_modification") {
        return fail(
            "download-only remote deletion did not preserve a local conflict"
        );
    }

    graph.changes.clear();
    config.delete_policy = onedrive::sync::DeletePolicy::preserve;
    if (onedrive::sync::SyncEngine{
            config, graph, items, metrics, &default_console
        }
                .synchronize() != exit_code::success ||
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
    onedrive::test::ScenarioProgress progress{7};
    if (const auto result = progress.run(
            "missing_file_delta_echo", test_missing_file_delta_echo
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    for (const bool retry_blocked : {false, true}) {
        if (const auto result = progress.run(
                std::string{"modified_file_delta_echo ("} +
                    "retry_blocked=" + std::to_string(retry_blocked) + ")",
                [&] { return test_modified_file_delta_echo(retry_blocked); }
            );
            result != EXIT_SUCCESS) {
            return result;
        }
    }
    if (const auto result = progress.run(
            "unchanged_echo_preserves_path_conflicts",
            test_unchanged_echo_preserves_path_conflicts
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = progress.run("upload_only", test_upload_only);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = progress.run(
            "download_only_propagates_remote_deletions",
            test_download_only_propagates_remote_deletions
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    return progress.finish(progress.run(
        "download_only_preserves_and_untracks_local_items",
        test_download_only_preserves_and_untracks_local_items
    ));
}
