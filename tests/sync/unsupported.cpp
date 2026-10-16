#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/storage/item_database.hpp"
#include "onedrive/util/sha256.hpp"
#include "onedrive/sync/blocked_policy.hpp"

namespace {

using namespace onedrive;
using namespace onedrive::test::sync;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

class CaptureObserver final : public events::Observer {
public:
    mutable std::vector<events::Event> captured;
    std::stop_source* stop_at_execution{nullptr};

    bool has_message(std::string_view event_name, std::string_view text) const {
        const std::scoped_lock lock{mutex_};
        return std::ranges::any_of(captured, [&](const auto& event) {
            const auto* message = std::get_if<events::MessageEvent>(&event);
            return message && message->event == event_name &&
                   message->kind == events::MessageKind::information &&
                   message->text.contains(text);
        });
    }

    bool field(std::string_view key, std::string_view value) const {
        const std::scoped_lock lock{mutex_};
        return std::ranges::any_of(captured, [&](const auto& event) {
            const auto* section = std::get_if<events::SectionEvent>(&event);
            return section &&
                   std::ranges::any_of(section->fields, [&](const auto& field) {
                       return field.key == key && field.value == value;
                   });
        });
    }

private:
    void on_event(const events::Event& event) const override {
        if (const auto* message = std::get_if<events::MessageEvent>(&event);
            stop_at_execution && message &&
            message->event == "execution_started") {
            stop_at_execution->request_stop();
        }
        const std::scoped_lock lock{mutex_};
        captured.push_back(event);
    }
    mutable std::mutex mutex_;
};

struct Fixture {
    test::TemporaryDirectory temporary;
    config::Config config{config_for(temporary.path() / "sync", false)};
    FakeMetrics metrics;
    CaptureObserver observer;

    account::DriveIdentity identity() const {
        return test::test_drive_identity(config.drive_id, config.drive_id);
    }

    int run(FakeGraphClient& graph, std::stop_token token = {}) {
        observer.captured.clear();
        storage::ItemDatabase database{config.state_directory, identity()};
        database.open();
        return sync::SyncEngine{config, graph, database, metrics, &observer}
            .synchronize(token);
    }
};

graph::RemoteItem supported_file(std::string id, std::string path) {
    auto item = file(std::move(id), std::move(path), 4);
    item.content_hash = util::FileHash{
        util::FileHashAlgorithm::sha1,
        "a17c9aaa61e80a1bf71d0d850af4e5baa9800bbd"
    };
    return item;
}

void test_existing_notes_and_durable_retries() {
    Fixture fixture;
    fixture.config.sync_mode = sync::SyncMode::bidirectional;
    fixture.config.local_conflict = config::LocalConflictPolicy::backup;
    fixture.config.force_large_delete = true;
    const auto notes_path = fixture.config.sync_data_directory / "notes.one";
    std::filesystem::create_directories(notes_path.parent_path());
    test::write_file(notes_path, "keep notes");
    {
        storage::ItemDatabase database{
            fixture.config.state_directory, fixture.identity()
        };
        database.open();
        database.apply_delta({
            .drive_id = fixture.config.drive_id,
            .upserts = {{
                .drive_id = fixture.config.drive_id,
                .remote_id = "notes",
                .name = "notes.one",
                .etag = "old-etag",
                .remote_path = "notes.one",
                .local_path = notes_path,
                .size = 10,
                .local_size = 10,
            }},
            .delta_link = "https://graph.example.test/old",
            .sync_filter_fingerprint = "old-format-policy",
        });
    }
    FakeGraphClient graph;
    auto notes = file("notes", "notes.one", 1'000'000'000);
    notes.unsupported_format = "onenote";
    notes.content_hash =
        util::FileHash{util::FileHashAlgorithm::sha1, std::string(40, '0')};
    graph.changes = {notes, supported_file("normal", "normal.txt")};
    graph.contents["normal"] = "data";
    require(
        fixture.run(graph) == exit_code::blocked &&
            graph.delta_requests ==
                std::vector<std::optional<std::string>>{std::nullopt} &&
            graph.download_count == 1 && graph.upload_count == 0 &&
            test::read_file(notes_path) == "keep notes" &&
            test::read_file(
                fixture.config.sync_data_directory / "normal.txt"
            ) == "data" &&
            fixture.observer.field("download_files", "1") &&
            fixture.observer.field("download_bytes", "4") &&
            fixture.observer.field("downloaded", "1") &&
            fixture.observer.field("unsupported_items", "1"),
        "unsupported note interrupted other work, counted as downloaded, or "
        "changed local data"
    );
    {
        storage::ItemDatabase database{
            fixture.config.state_directory, fixture.identity()
        };
        database.open();
        const auto blocked = database.blocked_items(fixture.config.drive_id);
        require(
            database.find(fixture.config.drive_id, "notes").has_value() &&
                blocked.size() == 1 &&
                blocked[0].reason_code == "unsupported_onenote" &&
                blocked[0].reason_message.contains("OneNote") &&
                database.delta_link(fixture.config.drive_id) ==
                    "https://graph.example.test/delta" &&
                !database.pending_delta(fixture.config.drive_id),
            "unsupported note was not reported durably or its old snapshot was "
            "lost"
        );
    }
    FakeGraphClient retry;
    require(
        fixture.run(retry) == exit_code::blocked && retry.download_count == 0 &&
            retry.upload_count == 0 &&
            fixture.observer.field("unsupported_items", "1") &&
            test::read_file(notes_path) == "keep notes",
        "persisted unsupported note was retried as a transfer"
    );
    FakeGraphClient deleted;
    deleted.changes = {{.id = "notes", .deleted = true}};
    require(
        fixture.run(deleted) == exit_code::blocked &&
            deleted.download_count == 0 && deleted.upload_count == 0 &&
            test::read_file(notes_path) == "keep notes",
        "remote deletion removed an unsupported local note"
    );
    FakeGraphClient refresh;
    refresh.reject_saved_cursor = true;
    require(
        fixture.run(refresh) == exit_code::blocked &&
            refresh.delta_requests.size() == 2 &&
            test::read_file(notes_path) == "keep notes",
        "full-refresh reconciliation removed an unsupported local note"
    );
    const auto renamed_path = notes_path.parent_path() / "renamed.one";
    std::filesystem::rename(notes_path, renamed_path);
    FakeGraphClient local_move;
    require(
        fixture.run(local_move) == exit_code::blocked &&
            local_move.moved_remote_items.empty() &&
            local_move.upload_count == 0 &&
            test::read_file(renamed_path) == "keep notes",
        "local rename or configurable conflict/deletion options bypassed the "
        "fixed format strategy"
    );
}

void test_package_children_and_ordinary_one_files() {
    Fixture fixture;
    const auto root = fixture.config.sync_data_directory;
    std::filesystem::create_directories(root / "Notebook/Sub");
    test::write_file(root / "Notebook/Sub/data.bin", "keep");
    auto package = file("package", "Notebook", 0);
    package.directory = true;
    package.unsupported_format = "onenote";
    auto middle = file("middle", "Notebook/Sub", 0);
    middle.directory = true;
    middle.parent_id = "package";
    auto child = file("child", "Notebook/Sub/data.bin", 4);
    child.parent_id = "middle";
    auto id_only = file("id-only", "detached.bin", 4);
    id_only.parent_id = "middle";
    auto path_only = file("path-only", "Notebook/Sub/no-parent.bin", 4);
    path_only.parent_id.clear();
    FakeGraphClient graph;
    graph.changes = {
        child,
        id_only,
        path_only,
        middle,
        package,
        supported_file("sibling", "Notebook-copy.bin"),
        supported_file("ordinary", "backup.one"),
    };
    graph.contents = {{"sibling", "data"}, {"ordinary", "data"}};
    require(
        fixture.run(graph) == exit_code::blocked && graph.download_count == 2 &&
            fixture.observer.field("unsupported_items", "5") &&
            fixture.observer.field("download_files", "2") &&
            fixture.observer.field("create_directories", "0") &&
            test::read_file(root / "Notebook/Sub/data.bin") == "keep" &&
            test::read_file(root / "backup.one") == "data" &&
            test::read_file(root / "Notebook-copy.bin") == "data" &&
            !std::filesystem::exists(root / "detached.bin") &&
            !std::filesystem::exists(root / "Notebook/Sub/no-parent.bin"),
        "package descendants were transferred or ordinary .one/prefix siblings "
        "were skipped"
    );
}

void test_persisted_ancestry_and_drive_isolation() {
    Fixture fixture;
    fixture.config.dry_run = true;
    {
        storage::ItemDatabase database{
            fixture.config.state_directory, fixture.identity()
        };
        database.open();
        storage::BlockedItem package;
        package.drive_id = fixture.config.drive_id;
        package.remote_id = "package";
        package.name = package.remote_path = "Notebook";
        package.directory = true;
        package.reason_code = "unsupported_onenote";
        package.reason_message = "Unsupported OneNote notebook";
        database.save_blocked_item(package);
        const auto filter = sync::detail::SyncList::configured({
            .rules_path = fixture.config.sync_list,
            .sync_root = fixture.config.sync_data_directory,
            .include_root_files = fixture.config.sync_root_files,
            .nosync_enabled = fixture.config.nosync_enabled,
            .dotfiles = fixture.config.dotfiles,
            .maximum_file_size_bytes = fixture.config.maximum_file_size_bytes,
        });
        database.apply_delta({
            .drive_id = fixture.config.drive_id,
            .upserts = {{
                .drive_id = fixture.config.drive_id,
                .remote_id = "middle",
                .parent_id = "package",
                .name = "Sub",
                .remote_path = "Notebook/Sub",
                .local_path =
                    fixture.config.sync_data_directory / "Notebook/Sub",
                .directory = true,
            }},
            .delta_link = "https://graph.example.test/seed",
            .sync_filter_fingerprint =
                sync::engine_detail::sync_scope_fingerprint(
                    filter.fingerprint()
                ),
        });
    }
    auto recycle = file("recycle", "Notebook/Sub/OneNote_RecycleBin", 0);
    recycle.directory = true;
    recycle.parent_id = "middle";
    auto child =
        supported_file("child", "Notebook/Sub/OneNote_RecycleBin/data.bin");
    child.parent_id = "recycle";
    FakeGraphClient graph;
    graph.changes = {child, recycle};
    require(
        fixture.run(graph) == exit_code::blocked &&
            fixture.observer.field("unsupported_items", "3") &&
            fixture.observer.field("download_files", "0"),
        "a partial delta bypassed persisted OneNote ancestry"
    );
    {
        storage::ItemDatabase database{
            fixture.config.state_directory, fixture.identity()
        };
        database.open();
        require(
            database.blocked_items(fixture.config.drive_id).size() == 3 &&
                database.delta_link(fixture.config.drive_id) ==
                    "https://graph.example.test/seed" &&
                graph.download_count == 0 && graph.upload_count == 0 &&
                !std::filesystem::exists(fixture.config.sync_data_directory),
            "ancestry preview lost blockers or changed synchronized state"
        );
    }

    FakeItemStore store;
    storage::ItemStore items{util::borrowed_proxy, store};
    for (const auto& drive : {"selected", "other"}) {
        storage::BlockedItem blocked;
        blocked.drive_id = drive;
        blocked.remote_id = std::string{drive} + "-package";
        blocked.remote_path = "Notebook";
        blocked.directory = true;
        blocked.reason_code = "unsupported_onenote";
        store.blocked.push_back(blocked);
        storage::ItemState directory;
        directory.drive_id = drive;
        directory.remote_id = std::string{drive} + "-middle";
        directory.remote_path = "Notebook/Sub";
        directory.directory = true;
        store.items.emplace(directory.remote_id, directory);
    }
    auto ignored_file = store.items.at("selected-middle");
    ignored_file.remote_id = "selected-file";
    ignored_file.directory = false;
    store.items.emplace(ignored_file.remote_id, ignored_file);
    auto ignored = store.blocked.front();
    ignored.remote_id = "selected-deleted";
    ignored.deleted = true;
    store.blocked.push_back(ignored);
    ignored.remote_id = "selected-file";
    ignored.deleted = false;
    ignored.directory = false;
    store.blocked.push_back(ignored);
    ignored.directory = true;
    for (const auto reason : {"malware_detected", "future_reason"}) {
        ignored.remote_id = reason;
        ignored.reason_code = reason;
        store.blocked.push_back(ignored);
    }
    const auto context = sync::blocked_ancestor_context("selected", items);
    require(
        context.size() == 2 && std::ranges::all_of(
                                   context,
                                   [](const auto& item) {
                                       return item.id.starts_with("selected-");
                                   }
                               ),
        "ancestor context leaked another Drive's same-path records"
    );
}

void test_ordinary_integrity_failure_remains_fatal() {
    Fixture fixture;
    FakeGraphClient graph;
    auto ordinary = supported_file("ordinary", "ordinary.one");
    ordinary.content_hash->value = std::string(40, '0');
    auto notes = file("notes", "notes.one", 4);
    notes.unsupported_format = "onenote";
    graph.changes = {notes, ordinary};
    graph.contents["ordinary"] = "data";
    bool integrity_failed = false;
    try {
        static_cast<void>(fixture.run(graph));
    } catch (const std::runtime_error& error) {
        integrity_failed =
            std::string_view{error.what()}.find("SHA-1 verification failed") !=
            std::string_view::npos;
    }
    require(
        integrity_failed && graph.download_count == 1 &&
            !std::filesystem::exists(
                fixture.config.sync_data_directory / "ordinary.one"
            ),
        "format policy bypassed an ordinary file's failed integrity check"
    );
    storage::ItemDatabase database{
        fixture.config.state_directory, fixture.identity()
    };
    database.open();
    const auto blocked = database.blocked_items(fixture.config.drive_id);
    require(
        blocked.size() == 1 && blocked[0].remote_id == "notes" &&
            !database.delta_link(fixture.config.drive_id),
        "later transfer failure lost an already discovered blocker"
    );
}

void test_preview_persists_diagnostics_without_committing_sync_state() {
    Fixture fixture;
    fixture.config.dry_run = true;
    FakeGraphClient graph;
    auto notes = file("notes", "notes.one", 4);
    notes.unsupported_format = "onenote";
    auto malware = file("malware", "danger.exe", 4);
    malware.malware = true;
    graph.changes = {notes, malware, file("invalid", "../outside", 4)};
    for (std::uint64_t count = 1; count <= 2; ++count) {
        require(
            fixture.run(graph) == exit_code::blocked,
            "preview accepted blocked remote metadata"
        );
        storage::ItemDatabase database{
            fixture.config.state_directory, fixture.identity()
        };
        database.open();
        const auto blocked = database.blocked_items(fixture.config.drive_id);
        require(
            blocked.size() == 3 &&
                std::ranges::all_of(
                    blocked,
                    [=](const auto& item) {
                        return item.attempt_count == count;
                    }
                ) &&
                database.drive_items(fixture.config.drive_id).empty() &&
                !database.delta_link(fixture.config.drive_id) &&
                !database.pending_delta(fixture.config.drive_id) &&
                graph.download_count == 0 && graph.upload_count == 0 &&
                !std::filesystem::exists(fixture.config.sync_data_directory),
            "preview duplicated/lost diagnostics or changed files, cursor, or "
            "snapshots"
        );
    }
    fixture.config.dry_run = false;
    graph.changes = {supported_file("malware", "safe.txt")};
    graph.contents["malware"] = "data";
    require(
        fixture.run(graph) == exit_code::blocked,
        "full refresh failed to preserve unsupported notes"
    );
    storage::ItemDatabase database{
        fixture.config.state_directory, fixture.identity()
    };
    database.open();
    const auto blocked = database.blocked_items(fixture.config.drive_id);
    require(
        blocked.size() == 1 && blocked[0].remote_id == "notes" &&
            test::read_file(fixture.config.sync_data_directory / "safe.txt") ==
                "data",
        "confirmed supported/deleted items retained obsolete blockers"
    );
}

void test_pending_restart_keeps_format_classification() {
    Fixture fixture;
    const auto notes_path = fixture.config.sync_data_directory / "notes.one";
    std::filesystem::create_directories(notes_path.parent_path());
    test::write_file(notes_path, "keep notes");
    FakeGraphClient first;
    auto notes = file("notes", "notes.one", 4);
    notes.unsupported_format = "onenote";
    first.changes = {notes};
    std::stop_source stop;
    fixture.observer.stop_at_execution = &stop;
    require(
        fixture.run(first, stop.get_token()) == exit_code::cancelled,
        "cannot interrupt a fetched unsupported batch"
    );
    fixture.observer.stop_at_execution = nullptr;
    {
        storage::ItemDatabase database{
            fixture.config.state_directory, fixture.identity()
        };
        database.open();
        const auto pending = database.pending_delta(fixture.config.drive_id);
        require(
            pending && pending->delta.changes.size() == 1 &&
                pending->delta.changes[0].unsupported_format == "onenote" &&
                database.blocked_items(fixture.config.drive_id).size() == 1 &&
                !database.delta_link(fixture.config.drive_id),
            "cancellation lost discovered blockers or advanced the cursor"
        );
    }
    FakeGraphClient resumed;
    resumed.delta_override = [](const auto& cursor) {
        require(
            cursor == "https://graph.example.test/delta",
            "unsupported pending batch was unnecessarily refetched"
        );
        return graph::DeltaResult{
            .changes = {},
            .delta_link = "https://graph.example.test/latest",
        };
    };
    require(
        fixture.run(resumed) == exit_code::blocked &&
            resumed.download_count == 0 &&
            fixture.observer.field("unsupported_items", "1") &&
            test::read_file(notes_path) == "keep notes",
        "restart transferred unsupported content or lost the local file"
    );
}

void test_unknown_blockers_are_not_retried_or_cleared() {
    Fixture fixture;
    fixture.config.sync_mode = sync::SyncMode::bidirectional;
    std::filesystem::create_directories(fixture.config.sync_data_directory);
    test::write_file(fixture.config.sync_data_directory / "local.txt", "keep");
    {
        storage::ItemDatabase database{
            fixture.config.state_directory, fixture.identity()
        };
        database.open();
        storage::BlockedItem blocked;
        blocked.drive_id = fixture.config.drive_id;
        blocked.remote_id = "mystery";
        blocked.remote_path = "mystery.txt";
        blocked.reason_code = "future_blocker";
        blocked.reason_message = "Unknown policy";
        database.save_blocked_item(blocked);
        blocked.remote_id = "upload:unknown";
        blocked.remote_path = "local.txt";
        blocked.reason_code = "upload_future";
        database.save_blocked_item(blocked);
    }
    for (const bool fresh_change : {false, true}) {
        FakeGraphClient graph;
        if (fresh_change) {
            graph.changes = {supported_file("mystery", "mystery.txt")};
            graph.contents["mystery"] = "data";
        }
        require(
            fixture.run(graph) == exit_code::blocked &&
                graph.download_count == 0 && graph.upload_count == 0 &&
                test::read_file(
                    fixture.config.sync_data_directory / "local.txt"
                ) == "keep" &&
                !std::filesystem::exists(
                    fixture.config.sync_data_directory / "mystery.txt"
                ),
            "unknown blocker was silently bypassed by a refresh or local scan"
        );
    }
    storage::ItemDatabase database{
        fixture.config.state_directory, fixture.identity()
    };
    database.open();
    require(
        database.blocked_items(fixture.config.drive_id).size() == 2,
        "unknown blockers were cleared without a recognized resolution policy"
    );
}

void test_fixed_policies_guard_pending_recovery() {
    Fixture fixture;
    fixture.config.sync_mode = sync::SyncMode::bidirectional;
    const auto root = fixture.config.sync_data_directory;
    std::filesystem::create_directories(root);
    const auto temporary = root / ".notes.one.onedrive-partial-recovery";
    const auto snapshot = root / ".local.txt.onedrive-upload-recovery";
    test::write_file(temporary, "data");
    test::write_file(snapshot, "data");
    test::write_file(root / "local.txt", "data");
    {
        storage::ItemDatabase database{
            fixture.config.state_directory, fixture.identity()
        };
        database.open();
        storage::BlockedItem blocked;
        blocked.drive_id = fixture.config.drive_id;
        blocked.remote_id = "notes";
        blocked.remote_path = "notes.one";
        blocked.reason_code = "unsupported_onenote";
        blocked.reason_message = "Unsupported OneNote file";
        database.save_blocked_item(blocked);
        database.save_pending_download({
            .item =
                {
                    .drive_id = fixture.config.drive_id,
                    .remote_id = "notes",
                    .etag = "etag",
                    .remote_path = "notes.one",
                    .local_path = root / "notes.one",
                    .last_modified = "2026-10-02T00:00:00Z",
                    .size = 4,
                },
            .temporary_path = temporary,
            .content_fingerprint = util::sha256_hex("data"),
        });
        blocked.remote_id = "notes-move";
        blocked.remote_path = "move.one";
        database.save_blocked_item(blocked);
        database.save_pending_remote_move({
            .drive_id = fixture.config.drive_id,
            .remote_id = "notes-move",
            .expected_etag = "etag",
            .source_remote_path = "old.one",
            .destination_remote_path = "move.one",
            .source_local_path = root / "old.one",
            .destination_local_path = root / "move.one",
            .local_device = 1,
            .local_inode = 2,
        });
        blocked.remote_id = "upload:unknown";
        blocked.remote_path = "local.txt";
        blocked.reason_code = "upload_future";
        blocked.reason_message = "Unrecognized upload policy";
        database.save_blocked_item(blocked);
        database.save_pending_upload({
            .drive_id = fixture.config.drive_id,
            .remote_path = "local.txt",
            .local_path = root / "local.txt",
            .snapshot_path = snapshot,
            .content_fingerprint = util::sha256_hex("data"),
            .local_size = 4,
        });
    }
    FakeGraphClient graph;
    require(
        fixture.run(graph) == exit_code::blocked && graph.upload_count == 0 &&
            graph.download_count == 0 &&
            !std::filesystem::exists(root / "notes.one") &&
            std::filesystem::exists(temporary) &&
            std::filesystem::exists(snapshot),
        "pending recovery bypassed a fixed or unknown blocking strategy"
    );
    storage::ItemDatabase database{
        fixture.config.state_directory, fixture.identity()
    };
    database.open();
    require(
        database.pending_downloads(fixture.config.drive_id).size() == 1 &&
            database.pending_uploads(fixture.config.drive_id).size() == 1 &&
            database.pending_remote_moves(fixture.config.drive_id).size() == 1,
        "blocked recovery discarded journals instead of deferring them"
    );
}

void test_root_remote_deletion_continues_with_blocked_notes() {
    for (const auto mode :
         {sync::SyncMode::download_only, sync::SyncMode::bidirectional}) {
        Fixture fixture;
        fixture.config.sync_mode = mode;
        const std::string filename = "Root document (version 2).pdf";
        const auto local_path = fixture.config.sync_data_directory / filename;
        FakeGraphClient initial;
        auto notes = file("notes", "Notes/unsupported.one", 4);
        notes.unsupported_format = "onenote";
        initial.changes = {supported_file("pdf", filename), notes};
        initial.contents["pdf"] = "data";
        require(
            fixture.run(initial) == exit_code::blocked &&
                test::read_file(local_path) == "data",
            "blocked notes interrupted downloading an ordinary root file"
        );
        fixture.config.dry_run = true;
        FakeGraphClient preview;
        preview.changes = {deleted_item("pdf")};
        require(
            fixture.run(preview) == exit_code::blocked &&
                std::filesystem::exists(local_path) &&
                !fixture.observer.has_message("local_item_removed", filename) &&
                fixture.observer.field("local_removals", "1"),
            "preview applied or lost an unrelated root deletion beside blocked "
            "notes"
        );
        fixture.config.dry_run = false;
        FakeGraphClient apply;
        apply.changes = {deleted_item("pdf")};
        require(
            fixture.run(apply) == exit_code::blocked &&
                !std::filesystem::exists(local_path) &&
                fixture.observer.has_message(
                    "local_item_removed", local_path.string()
                ) &&
                apply.upload_count == 0 && apply.deleted_items.empty() &&
                fixture.observer.field("local_removals", "1"),
            "blocked notes prevented an ordinary root deletion or resurrected "
            "it remotely"
        );
        storage::ItemDatabase database{
            fixture.config.state_directory, fixture.identity()
        };
        database.open();
        const auto blocked = database.blocked_items(fixture.config.drive_id);
        require(
            !database.find(fixture.config.drive_id, "pdf") &&
                blocked.size() == 1 &&
                blocked[0].reason_code == "unsupported_onenote" &&
                !database.pending_delta(fixture.config.drive_id),
            "root deletion did not commit independently of unrelated blocked "
            "notes"
        );
    }
}

} // namespace

int main() {
    try {
        test_existing_notes_and_durable_retries();
        test_package_children_and_ordinary_one_files();
        test_persisted_ancestry_and_drive_isolation();
        test_ordinary_integrity_failure_remains_fatal();
        test_preview_persists_diagnostics_without_committing_sync_state();
        test_pending_restart_keeps_format_classification();
        test_unknown_blockers_are_not_retried_or_cleared();
        test_fixed_policies_guard_pending_recovery();
        test_root_remote_deletion_continues_with_blocked_notes();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        return test::fail(error.what());
    }
}
