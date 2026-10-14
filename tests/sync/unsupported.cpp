#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/storage/item_database.hpp"

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

} // namespace

int main() {
    try {
        test_existing_notes_and_durable_retries();
        test_package_children_and_ordinary_one_files();
        test_ordinary_integrity_failure_remains_fatal();
        test_preview_persists_diagnostics_without_committing_sync_state();
        test_pending_restart_keeps_format_classification();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        return test::fail(error.what());
    }
}
