#include "support/progress.hpp"
#include "support.hpp"

#include "onedrive/exit_code.hpp"
#include "onedrive/storage/item_database.hpp"
#include "onedrive/sync/cancellation.hpp"
#include "onedrive/sync/download/restore.hpp"
#include "onedrive/ui/cli/console.hpp"
#include "sync/download/target.hpp"

#include <algorithm>
#include <limits>
#include <tuple>

namespace {

using namespace onedrive::test::download;
namespace exit_code = onedrive::exit_code;
namespace storage = onedrive::storage;

struct Fixture {
    TemporaryDirectory temporary;
    std::filesystem::path root{temporary.path() / "sync"};
    onedrive::config::Config config{onedrive::config::Config::defaults()};
    FakeGraphClient remote;
    onedrive::graph::GraphClient graph{onedrive::util::borrowed_proxy, remote};
    storage::ItemDatabase database{
        temporary.path() / "state", onedrive::test::test_drive_identity()
    };
    storage::ItemStore items{onedrive::util::borrowed_proxy, database};
    std::ostringstream output;
    std::ostringstream error;
    onedrive::cli::Console console{
        {.ui = onedrive::cli::UiMode::console}, output, error
    };

    Fixture() {
        config.sync_data_directory = root;
        config.drive_id = "drive-id";
        config.sync_root_files = true;
        config.download_concurrency = 1;
        config.filesystem_metadata =
            onedrive::config::FilesystemMetadataMode::database;
        config.local_conflict = onedrive::config::LocalConflictPolicy::backup;
        database.open();
        database.apply_delta({
            .drive_id = config.drive_id,
            .delta_link = "original-cursor",
            .sync_filter_fingerprint = "original-filter",
        });
        database.save_pending_delta({
            .drive_id = config.drive_id,
            .scope_fingerprint = "pending-scope",
            .delta = {.changes = {}, .delta_link = "pending-cursor"},
        });
    }

    int run(const std::string& scope = {}) {
        return onedrive::sync::restore_missing_files(
            config, scope, graph, items, console
        );
    }

    bool cursor_preserved() const {
        const auto pending = database.pending_delta(config.drive_id);
        return database.delta_link(config.drive_id) == "original-cursor" &&
               database.sync_filter_fingerprint(config.drive_id) ==
                   "original-filter" &&
               pending && pending->delta.delta_link == "pending-cursor" &&
               remote.delete_count == 0 && !remote.last_delta_cursor;
    }
};

onedrive::graph::RemoteItem directory(std::string id, std::string path) {
    auto result = remote_item(std::move(id), std::move(path));
    result.directory = true;
    result.size = 0;
    return result;
}

int test_preview_and_scoped_restore() {
    Fixture fixture;
    std::filesystem::create_directories(fixture.root / "Docs");
    onedrive::test::write_file(
        fixture.root / "Docs/existing.txt", "local edits"
    );
    fixture.remote.delta_result = {
        .changes =
            {
                directory("docs", "Docs"),
                remote_item("missing", "Docs/missing.txt"),
                remote_item("existing", "Docs/existing.txt"),
                directory("empty", "Docs/Empty"),
                remote_item("unrelated", "Docs-other/unrelated.txt"),
            },
        .delta_link = "must-not-apply",
    };
    fixture.database.save_pending_delete({
        .drive_id = fixture.config.drive_id,
        .remote_id = "docs",
        .expected_etag = "etag",
        .remote_path = "Docs",
        .local_path = fixture.root / "Docs",
        .directory = true,
    });
    fixture.database.save_pending_delete({
        .drive_id = fixture.config.drive_id,
        .remote_id = "other",
        .expected_etag = "etag",
        .remote_path = "Other",
        .local_path = fixture.root / "Other",
        .directory = true,
    });
    fixture.config.dry_run = true;
    storage::ItemDatabase read_only{
        fixture.temporary.path() / "state",
        onedrive::test::test_drive_identity()
    };
    read_only.open_read_only();
    storage::ItemStore read_only_items{
        onedrive::util::borrowed_proxy, read_only
    };
    if (onedrive::sync::restore_missing_files(
            fixture.config,
            "Docs",
            fixture.graph,
            read_only_items,
            fixture.console
        ) != exit_code::success ||
        fixture.remote.download_count != 0 ||
        std::filesystem::exists(fixture.root / "Docs/missing.txt") ||
        std::filesystem::exists(fixture.root / "Docs/Empty") ||
        fixture.database.size() != 0 ||
        fixture.database.pending_deletes(fixture.config.drive_id).size() != 2 ||
        !fixture.cursor_preserved() ||
        !fixture.output.str().contains("Docs/missing.txt") ||
        !fixture.output.str().contains("pending cloud deletes to cancel: 1")) {
        return fail(
            "restore preview changed files, journals or cursor, or lost its "
            "plan"
        );
    }
    fixture.config.dry_run = false;
    if (fixture.run("Docs") != exit_code::success ||
        onedrive::test::read_file(fixture.root / "Docs/missing.txt") !=
            "data" ||
        onedrive::test::read_file(fixture.root / "Docs/existing.txt") !=
            "local edits" ||
        !std::filesystem::is_directory(fixture.root / "Docs/Empty") ||
        std::filesystem::exists(fixture.root / "Docs-other") ||
        fixture.remote.download_count != 1 ||
        fixture.database.pending_deletes(fixture.config.drive_id).size() != 1 ||
        fixture.database.pending_deletes(fixture.config.drive_id)[0]
                .remote_id != "other" ||
        !fixture.database.find(fixture.config.drive_id, "missing") ||
        !fixture.cursor_preserved()) {
        return fail(
            "scoped restore changed existing files, unrelated scope or cloud "
            "state"
        );
    }
    if (fixture.run("Docs") != exit_code::success ||
        fixture.remote.download_count != 1 || !fixture.cursor_preserved()) {
        return fail("repeated restore downloaded existing content");
    }
    return EXIT_SUCCESS;
}

int test_whole_drive_filters_and_missing_cloud() {
    Fixture fixture;
    const auto rules = fixture.temporary.path() / "sync-list";
    onedrive::test::write_file(rules, "Docs/\n!Docs/excluded.txt\n");
    fixture.config.sync_list = rules;
    fixture.remote.delta_result.changes = {
        directory("docs", "Docs"),
        remote_item("included", "Docs/included.txt"),
        remote_item("excluded", "Docs/excluded.txt"),
        remote_item("outside", "Other/outside.txt"),
    };
    if (fixture.run() != exit_code::success ||
        onedrive::test::read_file(fixture.root / "Docs/included.txt") !=
            "data" ||
        std::filesystem::exists(fixture.root / "Docs/excluded.txt") ||
        std::filesystem::exists(fixture.root / "Other") ||
        fixture.remote.download_count != 1 || !fixture.cursor_preserved()) {
        return fail(
            "Drive-wide restoration bypassed sync filters or changed cloud "
            "state"
        );
    }
    auto removed = item_state(
        remote_item("removed", "Docs/removed.txt"),
        fixture.root / "Docs/removed.txt"
    );
    removed.drive_id = fixture.config.drive_id;
    fixture.database.upsert(removed);
    if (fixture.run("Docs/removed.txt") != exit_code::blocked ||
        !fixture.error.str().contains("recycle bin") ||
        fixture.remote.download_count != 1 ||
        std::filesystem::exists(removed.local_path)) {
        return fail(
            "missing cloud content was reported as a successful restoration"
        );
    }
    return EXIT_SUCCESS;
}

int test_file_scope_and_path_safety() {
    Fixture fixture;
    fixture.remote.delta_result.changes = {
        remote_item("file", "one.txt"), remote_item("other", "two.txt")
    };
    if (fixture.run("one.txt") != exit_code::success ||
        onedrive::test::read_file(fixture.root / "one.txt") != "data" ||
        std::filesystem::exists(fixture.root / "two.txt") ||
        fixture.remote.download_count != 1) {
        return fail("single-file recovery restored the wrong scope");
    }
    const auto queries = fixture.remote.delta_count;
    for (const std::string path :
         {"../escape", "/absolute", "Docs/../one.txt"}) {
        if (!onedrive::test::throws_with<std::runtime_error>(
                [&] { static_cast<void>(fixture.run(path)); }, "remote"
            ) ||
            fixture.remote.delta_count != queries) {
            return fail("unsafe restore scope reached Graph");
        }
    }
    const auto outside = fixture.temporary.path() / "outside";
    std::filesystem::create_directory(outside);
    std::filesystem::create_directory_symlink(outside, fixture.root / "Link");
    fixture.remote.delta_result.changes = {
        remote_item("unsafe", "Link/file.txt")
    };
    if (!onedrive::test::throws_with<std::runtime_error>(
            [&] { static_cast<void>(fixture.run()); }, "symbolic"
        ) ||
        std::filesystem::exists(outside / "file.txt") ||
        fixture.remote.download_count != 1) {
        return fail("restoration followed a symbolic-link parent");
    }
    return EXIT_SUCCESS;
}

int test_concurrent_local_creation_and_journal_conflicts() {
    Fixture fixture;
    fixture.remote.delta_result.changes = {remote_item("race", "race.txt")};
    fixture.remote.before_download = [&] {
        onedrive::test::write_file(
            fixture.root / "race.txt", "concurrent edit"
        );
    };
    if (fixture.run() != exit_code::blocked ||
        onedrive::test::read_file(fixture.root / "race.txt") !=
            "concurrent edit" ||
        fixture.database.find(fixture.config.drive_id, "race") ||
        !fixture.cursor_preserved()) {
        return fail("restore overwrote a file created after planning");
    }
    fixture.remote.before_download = {};
    fixture.remote.delta_result.changes = {
        remote_item("pending", "Pending/file.txt")
    };
    fixture.database.save_pending_upload({
        .drive_id = fixture.config.drive_id,
        .remote_path = "Pending",
        .local_path = fixture.root / "Pending",
        .directory = true,
    });
    const auto downloads = fixture.remote.download_count;
    if (fixture.run() != exit_code::blocked ||
        fixture.remote.download_count != downloads ||
        fixture.database.pending_uploads(fixture.config.drive_id).size() != 1 ||
        std::filesystem::exists(fixture.root / "Pending") ||
        !fixture.cursor_preserved()) {
        return fail(
            "restore changed an overlapping journal or created its target"
        );
    }
    fixture.database.remove_pending_upload(fixture.config.drive_id, "Pending");
    const auto state = onedrive::sync::detail::item_state_for(
        fixture.config.drive_id,
        fixture.remote.delta_result.changes[0],
        fixture.root / "Pending/file.txt"
    );
    fixture.database.save_pending_download({
        .item = state,
        .temporary_path =
            fixture.root / "Pending/.file.txt.onedrive-partial-old",
        .content_fingerprint = std::string(64, 'a'),
    });
    if (fixture.run() != exit_code::blocked ||
        fixture.remote.download_count != downloads ||
        fixture.database.pending_downloads(fixture.config.drive_id).size() !=
            1) {
        return fail("restore replaced an unresolved installation journal");
    }
    fixture.database.remove_pending_download(
        fixture.config.drive_id, "pending"
    );
    fixture.database.save_pending_move({
        .drive_id = fixture.config.drive_id,
        .remote_id = "parent",
        .source_path = fixture.root / "Source",
        .destination_path = fixture.root / "Pending",
        .staging_path = fixture.root / ".Source.onedrive-move-old",
        .directory = true,
    });
    if (fixture.run() != exit_code::blocked ||
        fixture.remote.download_count != downloads ||
        fixture.database.pending_moves(fixture.config.drive_id).size() != 1) {
        return fail("restore crossed a pending local directory move");
    }
    fixture.database.remove_pending_move(fixture.config.drive_id, "parent");
    fixture.database.save_pending_remote_move({
        .drive_id = fixture.config.drive_id,
        .remote_id = "parent",
        .expected_etag = "parent-etag",
        .source_remote_path = "Source",
        .destination_remote_path = "Pending",
        .source_local_path = fixture.root / "Source",
        .destination_local_path = fixture.root / "Pending",
        .local_device = 1,
        .local_inode = 2,
        .directory = true,
    });
    if (fixture.run() != exit_code::blocked ||
        fixture.remote.download_count != downloads ||
        fixture.database.pending_remote_moves(fixture.config.drive_id).size() !=
            1 ||
        std::filesystem::exists(fixture.root / "Pending")) {
        return fail("restore crossed a pending remote directory move");
    }
    return EXIT_SUCCESS;
}

int test_partial_resume_and_blocked_items() {
    Fixture fixture;
    std::filesystem::create_directory(fixture.root);
    const auto partial = fixture.root / ".file.txt.onedrive-partial-previous";
    onedrive::test::write_file(partial, "dauncommitted");
    const auto file = remote_item("file", "file.txt");
    fixture.remote.delta_result.changes = {file};
    auto state = item_state(file, fixture.root / "file.txt");
    state.drive_id = fixture.config.drive_id;
    fixture.database.save_partial_download({
        .item = state,
        .temporary_path = partial,
        .completed_bytes = 2,
    });
    if (fixture.run() != exit_code::success ||
        fixture.remote.last_initial_offset != 2 ||
        onedrive::test::read_file(fixture.root / "file.txt") != "data" ||
        !fixture.database.partial_downloads(fixture.config.drive_id).empty() ||
        !fixture.cursor_preserved()) {
        return fail("restore did not reuse a safe partial checkpoint");
    }
    auto unsafe = remote_item("unsafe", "unsafe.txt");
    unsafe.malware = true;
    auto unsupported = remote_item("unsupported", "notes.one");
    unsupported.unsupported_format = "onenote";
    auto unknown = remote_item("unknown", "unknown.txt");
    fixture.database.save_blocked_item({
        .drive_id = fixture.config.drive_id,
        .remote_id = unknown.id,
        .remote_path = unknown.remote_path,
        .reason_code = "future_policy",
        .reason_message = "Unknown blocking policy",
    });
    fixture.remote.delta_result.changes = {unsafe, unsupported, unknown};
    if (fixture.run() != exit_code::blocked ||
        fixture.remote.download_count != 1 ||
        std::filesystem::exists(fixture.root / "unsafe.txt") ||
        std::filesystem::exists(fixture.root / "notes.one") ||
        std::filesystem::exists(fixture.root / "unknown.txt")) {
        return fail(
            "restore bypassed malware, unsupported format or unknown policy"
        );
    }
    return EXIT_SUCCESS;
}

int test_failures_and_cancellation() {
    for (const bool cancelled : {false, true}) {
        Fixture fixture;
        fixture.remote.delta_result.changes = {remote_item("file", "file.txt")};
        fixture.remote.before_download = [&] {
            if (cancelled) {
                throw onedrive::graph::DownloadCancelledError{
                    "simulated download cancellation"
                };
            }
            throw std::runtime_error{"simulated network failure"};
        };
        if (!onedrive::test::throws_with<std::runtime_error>(
                [&] { static_cast<void>(fixture.run()); },
                cancelled ? "did not complete" : "simulated network failure"
            ) ||
            std::filesystem::exists(fixture.root / "file.txt") ||
            fixture.database.find(fixture.config.drive_id, "file") ||
            fixture.output.str().contains("Restored ") ||
            !fixture.cursor_preserved()) {
            return fail("incomplete recovery was reported as success");
        }
    }
    Fixture fixture;
    std::stop_source stop;
    stop.request_stop();
    if (!onedrive::test::throws_with<onedrive::sync::SyncCancelledError>(
            [&] {
                static_cast<void>(onedrive::sync::restore_missing_files(
                    fixture.config,
                    {},
                    fixture.graph,
                    fixture.items,
                    fixture.console,
                    stop.get_token()
                ));
            },
            "cancelled"
        ) ||
        fixture.remote.delta_count != 0 ||
        std::filesystem::exists(fixture.root)) {
        return fail("cancelled recovery contacted Graph or created files");
    }
    return EXIT_SUCCESS;
}

int test_invalid_cloud_inventory() {
    auto negative = remote_item("negative", "negative.txt");
    negative.size = -1;
    auto huge = remote_item("huge", "huge.txt");
    huge.size = std::numeric_limits<std::int64_t>::max();
    auto second_huge = huge;
    second_huge.id = "second-huge";
    second_huge.remote_path = "second-huge.txt";
    for (const auto& [changes, message] : std::vector<
             std::pair<std::vector<onedrive::graph::RemoteItem>, std::string>>{
             {{negative}, "negative remote file size"},
             {{remote_item("first", "same.txt"),
               remote_item("second", "same.txt")},
              "same restore destination"},
             {{huge, second_huge, remote_item("overflow", "overflow.txt")},
              "restore download size overflow"},
         }) {
        Fixture fixture;
        fixture.remote.delta_result.changes = changes;
        fixture.database.save_pending_delete({
            .drive_id = fixture.config.drive_id,
            .remote_id = changes.front().id,
            .expected_etag = "etag",
            .remote_path = changes.front().remote_path,
            .local_path = fixture.root / changes.front().remote_path,
        });
        if (!onedrive::test::throws_with<std::runtime_error>(
                [&] { static_cast<void>(fixture.run()); }, message
            ) ||
            fixture.remote.download_count != 0 ||
            std::filesystem::exists(fixture.root) ||
            fixture.database.size() != 0 ||
            fixture.database.pending_deletes(fixture.config.drive_id).size() !=
                1 ||
            !fixture.cursor_preserved()) {
            return fail("invalid inventory partially applied a restore plan");
        }
    }
    return EXIT_SUCCESS;
}

int test_journal_overlap_boundaries() {
    struct MoveCase {
        std::string source;
        std::string destination;
        bool directory;
        bool same_id;
        bool blocked;
    };
    for (const bool remote_move : {false, true}) {
        for (const auto& operation : std::vector<MoveCase>{
                 {"Old", "New", false, true, true},
                 {"Docs/file.txt", "Moved/file.txt", false, false, true},
                 {"Old/file.txt", "Docs/file.txt", false, false, true},
                 {"Docs", "Moved", true, false, true},
                 {"Old", "Docs", true, false, true},
                 {"Docs-other", "Docs-other/New", true, false, false},
                 {"Docs-other/file.txt",
                  "Elsewhere/file.txt",
                  false,
                  false,
                  false},
             }) {
            Fixture fixture;
            fixture.remote.delta_result.changes = {
                remote_item("file", "Docs/file.txt")
            };
            const std::string id = operation.same_id ? "file" : "move";
            if (remote_move) {
                fixture.database.save_pending_remote_move({
                    .drive_id = fixture.config.drive_id,
                    .remote_id = id,
                    .expected_etag = "etag",
                    .source_remote_path = operation.source,
                    .destination_remote_path = operation.destination,
                    .source_local_path = fixture.root / operation.source,
                    .destination_local_path =
                        fixture.root / operation.destination,
                    .local_device = 1,
                    .local_inode = 2,
                    .directory = operation.directory,
                });
            } else {
                fixture.database.save_pending_move({
                    .drive_id = fixture.config.drive_id,
                    .remote_id = id,
                    .source_path = fixture.root / operation.source,
                    .destination_path = fixture.root / operation.destination,
                    .staging_path = fixture.root / ".move.onedrive-move-old",
                    .directory = operation.directory,
                });
            }
            if (fixture.run("Docs/file.txt") != (operation.blocked
                                                     ? exit_code::blocked
                                                     : exit_code::success) ||
                fixture.remote.download_count != (operation.blocked ? 0 : 1) ||
                std::filesystem::exists(fixture.root / "Docs/file.txt") ==
                    operation.blocked ||
                fixture.database.pending_moves(fixture.config.drive_id)
                        .size() != (remote_move ? 0U : 1U) ||
                fixture.database.pending_remote_moves(fixture.config.drive_id)
                        .size() != (remote_move ? 1U : 0U) ||
                !fixture.cursor_preserved()) {
                return fail(
                    "move-journal identity or source/destination scope was "
                    "ignored"
                );
            }
        }
    }
    for (const bool installation : {false, true}) {
        for (const auto& path :
             {"Docs", "Docs/file.txt", "Docs-other/file.txt"}) {
            Fixture fixture;
            fixture.remote.delta_result.changes = {
                remote_item("file", "Docs/file.txt")
            };
            const auto pending = remote_item("other-id", path);
            if (installation) {
                fixture.database.save_pending_download({
                    .item = onedrive::sync::detail::item_state_for(
                        fixture.config.drive_id, pending, fixture.root / path
                    ),
                    .temporary_path =
                        fixture.root / ".pending.onedrive-partial-old",
                    .content_fingerprint = std::string(64, 'a'),
                });
            } else {
                fixture.database.save_pending_upload({
                    .drive_id = fixture.config.drive_id,
                    .remote_path = path,
                    .local_path = fixture.root / path,
                    .snapshot_path =
                        fixture.temporary.path() / "upload-snapshot",
                    .content_fingerprint = std::string(64, 'a'),
                    .local_size = 4,
                });
            }
            const bool blocked =
                std::string_view{path} != "Docs-other/file.txt";
            if (fixture.run("Docs/file.txt") !=
                    (blocked ? exit_code::blocked : exit_code::success) ||
                fixture.remote.download_count != (blocked ? 0 : 1) ||
                std::filesystem::exists(fixture.root / "Docs/file.txt") ==
                    blocked ||
                fixture.database.pending_downloads(fixture.config.drive_id)
                        .size() != (installation ? 1U : 0U) ||
                fixture.database.pending_uploads(fixture.config.drive_id)
                        .size() != (installation ? 0U : 1U) ||
                std::filesystem::exists(
                    fixture.root / ".pending.onedrive-partial-old"
                ) ||
                !fixture.cursor_preserved()) {
                return fail(
                    "installation/upload path overlap used IDs or raw prefixes"
                );
            }
        }
    }
    Fixture fixture;
    fixture.remote.delta_result.changes = {directory("parent", "Docs")};
    fixture.database.save_pending_upload({
        .drive_id = fixture.config.drive_id,
        .remote_path = "Docs/Child",
        .local_path = fixture.root / "Docs/Child",
        .directory = true,
    });
    if (fixture.run("Docs") != exit_code::blocked ||
        fixture.database.pending_uploads(fixture.config.drive_id).size() != 1 ||
        fixture.remote.download_count != 0 ||
        std::filesystem::exists(fixture.root) || !fixture.cursor_preserved()) {
        return fail("directory recovery crossed a pending descendant upload");
    }
    return EXIT_SUCCESS;
}

int test_empty_missing_and_deleted_scope() {
    Fixture fixture;
    auto root = directory("root", "");
    root.root = true;
    fixture.remote.delta_result.changes = {root};
    if (fixture.run() != exit_code::success ||
        std::filesystem::exists(fixture.root) ||
        fixture.output.str().contains("Restored ")) {
        return fail(
            "empty Drive recovery created content or claimed a restore"
        );
    }
    if (!onedrive::test::throws_with<std::runtime_error>(
            [&] { static_cast<void>(fixture.run("unknown.txt")); },
            "restore path was not found"
        ) ||
        std::filesystem::exists(fixture.root)) {
        return fail(
            "unknown cloud scope was reported as a successful recovery"
        );
    }
    auto deleted = remote_item("deleted", "deleted.txt");
    deleted.deleted = true;
    fixture.remote.delta_result.changes.push_back(deleted);
    if (fixture.run("deleted.txt") != exit_code::blocked ||
        !fixture.error.str().contains("recycle bin") ||
        fixture.remote.download_count != 0 || fixture.database.size() != 0 ||
        std::filesystem::exists(fixture.root) || !fixture.cursor_preserved()) {
        return fail("deleted cloud content was restored or silently ignored");
    }
    return EXIT_SUCCESS;
}

int test_destination_types_and_uninspectable_snapshot() {
    Fixture fixture;
    std::filesystem::create_directories(fixture.root / "file.txt");
    onedrive::test::write_file(fixture.root / "Folder", "local file");
    std::filesystem::create_symlink("Folder", fixture.root / "link.txt");
    fixture.remote.delta_result.changes = {
        remote_item("file", "file.txt"),
        directory("folder", "Folder"),
        remote_item("link", "link.txt"),
    };
    if (fixture.run() != exit_code::blocked ||
        fixture.remote.download_count != 0 ||
        !std::filesystem::is_directory(fixture.root / "file.txt") ||
        !std::filesystem::is_symlink(fixture.root / "link.txt") ||
        onedrive::test::read_file(fixture.root / "Folder") != "local file" ||
        fixture.database.size() != 0 ||
        !fixture.error.str().contains("conflicting type") ||
        !fixture.cursor_preserved()) {
        return fail(
            "recovery changed a conflicting file, directory or symlink"
        );
    }
    std::filesystem::create_symlink("Loop", fixture.root / "Loop");
    auto previous = item_state(
        remote_item("old", "Loop/old.txt"), fixture.root / "Loop/old.txt"
    );
    previous.drive_id = fixture.config.drive_id;
    fixture.database.upsert(previous);
    fixture.remote.delta_result.changes.clear();
    if (!onedrive::test::throws_with<std::filesystem::filesystem_error>(
            [&] { static_cast<void>(fixture.run("Loop/old.txt")); },
            "cannot inspect restore destination"
        ) ||
        fixture.remote.download_count != 0 ||
        !fixture.database.find(fixture.config.drive_id, "old") ||
        !fixture.cursor_preserved()) {
        return fail("an inaccessible snapshot was treated as a missing file");
    }
    return EXIT_SUCCESS;
}

int test_pending_delete_matching_and_directory_only_restore() {
    for (const bool directory_only : {false, true}) {
        Fixture fixture;
        const std::string path =
            directory_only ? "Docs/Empty" : "Docs/file.txt";
        fixture.remote.delta_result.changes = {
            directory_only ? directory("target", path)
                           : remote_item("target", path),
        };
        for (const auto& [id, deletion_path, is_directory] :
             std::vector<std::tuple<std::string, std::string, bool>>{
                 {"target", "Old", directory_only},
                 {"same-path", path, directory_only},
                 {"parent", "Docs", true},
                 {"child", path + "/child.txt", false},
                 {"boundary", "Docs-other", true},
             }) {
            fixture.database.save_pending_delete({
                .drive_id = fixture.config.drive_id,
                .remote_id = id,
                .expected_etag = "etag",
                .remote_path = deletion_path,
                .local_path = fixture.root / deletion_path,
                .directory = is_directory,
            });
        }
        fixture.config.dry_run = true;
        if (fixture.run(path) != exit_code::success ||
            !fixture.output.str().contains(
                "pending cloud deletes to cancel: 3"
            ) ||
            fixture.database.pending_deletes(fixture.config.drive_id).size() !=
                5 ||
            std::filesystem::exists(fixture.root)) {
            return fail("preview miscounted or cancelled overlapping deletes");
        }
        fixture.config.dry_run = false;
        if (fixture.run(path) != exit_code::success) {
            return fail("file or empty-directory restoration was blocked");
        }
        const auto state =
            fixture.database.find(fixture.config.drive_id, "target");
        const auto remaining =
            fixture.database.pending_deletes(fixture.config.drive_id);
        if (!state || state->directory != directory_only ||
            state->local_device == 0 || state->local_inode == 0 ||
            remaining.size() != 2 ||
            !std::ranges::any_of(
                remaining,
                [](const auto& deletion) {
                    return deletion.remote_id == "child";
                }
            ) ||
            !std::ranges::any_of(
                remaining,
                [](const auto& deletion) {
                    return deletion.remote_id == "boundary";
                }
            ) ||
            fixture.remote.download_count != (directory_only ? 0 : 1) ||
            !fixture.cursor_preserved()) {
            return fail("restore cancelled unrelated deletes or lost identity");
        }
        if (directory_only) {
            if (!std::filesystem::is_empty(fixture.root / path)) {
                return fail(
                    "directory-only recovery did not create an empty directory"
                );
            }
        } else if (onedrive::test::read_file(fixture.root / path) != "data") {
            return fail("recovery installed incorrect file content");
        }
    }
    return EXIT_SUCCESS;
}

int test_policy_filters_and_retained_snapshots() {
    Fixture fixture;
    const auto rules = fixture.temporary.path() / "sync-list";
    onedrive::test::write_file(rules, "Docs/\n");
    fixture.config.sync_list = rules;
    fixture.config.sync_root_files = false;
    fixture.config.dotfiles = onedrive::config::DotfilePolicy::exclude;
    fixture.config.maximum_file_size_bytes = 4;
    std::filesystem::create_directories(fixture.root / "Docs/Blocked");
    onedrive::test::write_file(fixture.root / "Docs/Blocked/.nosync", "");
    auto large = remote_item("large", "Docs/large.txt");
    large.size = 5;
    fixture.remote.delta_result.changes = {
        directory("docs", "Docs"),
        remote_item("allowed", "Docs/allowed.txt"),
        remote_item("hidden", "Docs/.hidden"),
        remote_item("hidden-child", "Docs/.hidden-dir/file.txt"),
        remote_item("nosync", "Docs/Blocked/file.txt"),
        remote_item("root-file", "root.txt"),
        large,
    };
    for (const auto& item : {
             large,
             remote_item("absent-hidden", "Docs/.old-hidden"),
             remote_item("absent-nosync", "Docs/Blocked/old.txt"),
             remote_item("absent-root", "old-root.txt"),
             directory("absent-outside", "Other"),
         }) {
        auto previous = item_state(item, fixture.root / item.remote_path);
        previous.drive_id = fixture.config.drive_id;
        fixture.database.upsert(previous);
    }
    auto existing = item_state(
        remote_item("existing-marker", "Docs/marker.txt"),
        fixture.root / "Docs/marker.txt"
    );
    existing.drive_id = fixture.config.drive_id;
    existing.size = -1;
    fixture.database.upsert(existing);
    onedrive::test::write_file(existing.local_path, "local edits");
    if (fixture.run() != exit_code::success ||
        fixture.remote.download_count != 1 ||
        onedrive::test::read_file(fixture.root / "Docs/allowed.txt") !=
            "data" ||
        onedrive::test::read_file(existing.local_path) != "local edits" ||
        std::filesystem::exists(fixture.root / "Docs/.hidden") ||
        std::filesystem::exists(fixture.root / "Docs/.hidden-dir") ||
        std::filesystem::exists(fixture.root / "Docs/Blocked/file.txt") ||
        std::filesystem::exists(fixture.root / "Docs/large.txt") ||
        std::filesystem::exists(fixture.root / "root.txt") ||
        !fixture.error.str().empty() || fixture.database.size() != 7 ||
        !fixture.cursor_preserved()) {
        return fail(
            "restore bypassed policy filters or discarded old snapshots"
        );
    }
    return EXIT_SUCCESS;
}

int test_missing_partial_and_resolved_blocker() {
    Fixture fixture;
    const auto file = remote_item("file", "file.txt");
    auto state = item_state(file, fixture.root / "file.txt");
    state.drive_id = fixture.config.drive_id;
    fixture.remote.delta_result.changes = {file};
    fixture.database.save_partial_download({
        .item = state,
        .temporary_path = fixture.root / ".file.txt.onedrive-partial-removed",
        .completed_bytes = 2,
    });
    fixture.database.save_blocked_item({
        .drive_id = fixture.config.drive_id,
        .remote_id = file.id,
        .remote_path = file.remote_path,
        .reason_code = "local_modification",
        .reason_message = "Previously modified local file",
    });
    if (fixture.run() != exit_code::success ||
        fixture.remote.last_initial_offset != 0 ||
        fixture.remote.download_count != 1 ||
        onedrive::test::read_file(fixture.root / "file.txt") != "data" ||
        !fixture.database.partial_downloads(fixture.config.drive_id).empty() ||
        !fixture.database.blocked_items(fixture.config.drive_id).empty() ||
        !fixture.cursor_preserved()) {
        return fail(
            "missing partial was resumed or resolved blocker was retained"
        );
    }
    return EXIT_SUCCESS;
}

int test_inventory_failure_and_cancellation() {
    for (const bool cancelled : {false, true}) {
        Fixture fixture;
        std::stop_source stop;
        fixture.remote.before_delta = [&] {
            if (cancelled) {
                stop.request_stop();
            } else {
                throw std::runtime_error("simulated inventory failure");
            }
        };
        const auto invoke = [&] {
            static_cast<void>(onedrive::sync::restore_missing_files(
                fixture.config,
                {},
                fixture.graph,
                fixture.items,
                fixture.console,
                stop.get_token()
            ));
        };
        const bool failed =
            cancelled
                ? onedrive::test::throws_with<
                      onedrive::sync::SyncCancelledError>(invoke, "cancelled")
                : onedrive::test::throws_with<std::runtime_error>(
                      invoke, "simulated inventory failure"
                  );
        if (!failed || fixture.remote.delta_count != 1 ||
            fixture.remote.download_count != 0 ||
            std::filesystem::exists(fixture.root) ||
            fixture.output.str().contains("Restored ") ||
            !fixture.cursor_preserved()) {
            return fail("failed inventory fetch changed local or cloud state");
        }
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    onedrive::test::ScenarioProgress progress{14};
    if (progress.run(
            "preview_and_scoped_restore", test_preview_and_scoped_restore
        ) != EXIT_SUCCESS ||
        progress.run(
            "whole_drive_filters_and_missing_cloud",
            test_whole_drive_filters_and_missing_cloud
        ) != EXIT_SUCCESS ||
        progress.run(
            "file_scope_and_path_safety", test_file_scope_and_path_safety
        ) != EXIT_SUCCESS ||
        progress.run(
            "concurrent_local_creation_and_journal_conflicts",
            test_concurrent_local_creation_and_journal_conflicts
        ) != EXIT_SUCCESS ||
        progress.run(
            "failures_and_cancellation", test_failures_and_cancellation
        ) != EXIT_SUCCESS ||
        progress.run("invalid_cloud_inventory", test_invalid_cloud_inventory) !=
            EXIT_SUCCESS ||
        progress.run(
            "journal_overlap_boundaries", test_journal_overlap_boundaries
        ) != EXIT_SUCCESS ||
        progress.run(
            "empty_missing_and_deleted_scope",
            test_empty_missing_and_deleted_scope
        ) != EXIT_SUCCESS ||
        progress.run(
            "destination_types_and_uninspectable_snapshot",
            test_destination_types_and_uninspectable_snapshot
        ) != EXIT_SUCCESS ||
        progress.run(
            "pending_delete_matching_and_directory_only_restore",
            test_pending_delete_matching_and_directory_only_restore
        ) != EXIT_SUCCESS ||
        progress.run(
            "policy_filters_and_retained_snapshots",
            test_policy_filters_and_retained_snapshots
        ) != EXIT_SUCCESS ||
        progress.run(
            "missing_partial_and_resolved_blocker",
            test_missing_partial_and_resolved_blocker
        ) != EXIT_SUCCESS ||
        progress.run(
            "inventory_failure_and_cancellation",
            test_inventory_failure_and_cancellation
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return progress.finish(progress.run(
        "partial_resume_and_blocked_items",
        test_partial_resume_and_blocked_items
    ));
}
