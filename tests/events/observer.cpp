#include "onedrive/ui/common/observer.hpp"
#include "onedrive/ui/common/file_history.hpp"
#include "onedrive/exit_code.hpp"
#include "sync/support.hpp"

#include <array>
#include <thread>
#include <type_traits>
#include <variant>

namespace {

using namespace onedrive::events;
using onedrive::test::fail;
using onedrive::util::ProgressState;

static_assert(std::is_same_v<MessageKind, onedrive::logging::Severity>);

class CapturingObserver final : public Observer {
public:
    mutable std::vector<Event> events;
    bool reject{false};

    [[nodiscard]] std::vector<FileOperationEvent> file_operations() const {
        std::vector<FileOperationEvent> result;
        for (const auto& event : events) {
            if (const auto* operation =
                    std::get_if<FileOperationEvent>(&event)) {
                result.push_back(*operation);
            }
        }
        return result;
    }

private:
    void on_event(const Event& event) const override {
        if (reject) {
            throw std::runtime_error{"observer rejected event"};
        }
        events.push_back(event);
    }
};

int test_payloads() {
    CapturingObserver observer;
    std::string text{"Working"};
    std::vector<Field> fields{
        {.label = "Files:", .key = "files", .value = "12"},
    };
    observer.message(MessageKind::information, "phase", text);
    observer.section("summary", "Summary", fields);
    observer.delta_progress(2, 350, ProgressState::ongoing);
    observer.delta_summary({
        .pages = 3,
        .scanned_items = 412,
        .unique_changes = 400,
        .files = 300,
        .directories = 90,
        .deletions = 10,
    });
    observer.blocked_item("file.txt", "local_modification", "modified");
    observer.download_progress(
        1,
        2,
        5,
        10,
        ProgressState::ongoing,
        {
            .bytes_per_second = 2'048,
            .estimated_seconds_remaining = 65,
            .elapsed_milliseconds = 500,
        }
    );
    observer.end_download_progress();
    observer.operation_state(
        OperationKind::synchronization, OperationState::syncing
    );
    text = "Changed";
    fields.front().value = "0";

    if (observer.events.size() != 8) {
        return fail("observer did not publish every event");
    }
    const auto& message = std::get<MessageEvent>(observer.events[0]);
    const auto& section = std::get<SectionEvent>(observer.events[1]);
    const auto& delta = std::get<DeltaProgressEvent>(observer.events[2]);
    const auto& summary =
        std::get<DeltaSummaryEvent>(observer.events[3]).summary;
    const auto& blocked = std::get<BlockedItemEvent>(observer.events[4]);
    const auto& download = std::get<DownloadProgressEvent>(observer.events[5]);
    if (message.kind != MessageKind::information || message.event != "phase" ||
        message.text != "Working" || section.event != "summary" ||
        section.title != "Summary" || section.fields.size() != 1 ||
        section.fields[0].label != "Files:" ||
        section.fields[0].key != "files" || section.fields[0].value != "12" ||
        delta.pages != 2 || delta.items != 350 ||
        delta.state != ProgressState::ongoing || summary.pages != 3 ||
        summary.scanned_items != 412 || summary.unique_changes != 400 ||
        summary.files != 300 || summary.directories != 90 ||
        summary.deletions != 10 || blocked.path != "file.txt" ||
        blocked.reason_code != "local_modification" ||
        blocked.reason_message != "modified" || download.completed_files != 1 ||
        download.file_count != 2 || download.downloaded != 5 ||
        download.total != 10 || download.state != ProgressState::ongoing ||
        download.metrics.bytes_per_second != 2'048 ||
        download.metrics.estimated_seconds_remaining != 65 ||
        download.metrics.elapsed_milliseconds != 500 ||
        !std::holds_alternative<EndDownloadProgressEvent>(observer.events[6]) ||
        std::get<OperationStateEvent>(observer.events[7]).operation !=
            OperationKind::synchronization ||
        std::get<OperationStateEvent>(observer.events[7]).state !=
            OperationState::syncing) {
        return fail("observer changed or borrowed event payloads");
    }
    if (download_progress_percentage(1, 2, 10, 10, ProgressState::ongoing) !=
            99 ||
        download_progress_percentage(2, 2, 10, 10, ProgressState::completed) !=
            100 ||
        download_progress_percentage(0, 0, 0, 0, ProgressState::ongoing) != 0 ||
        download_progress_percentage(0, 0, 0, 0, ProgressState::completed) !=
            100) {
        return fail("neutral progress percentage changed completion semantics");
    }
    return EXIT_SUCCESS;
}

int test_workers_and_errors() {
    CapturingObserver observer;
    {
        std::array<std::jthread, 4> workers;
        for (auto& worker : workers) {
            worker = std::jthread{[&observer] {
                for (int index = 0; index < 100; ++index) {
                    observer.message(
                        MessageKind::information,
                        "worker",
                        std::to_string(index)
                    );
                }
            }};
        }
    }
    if (observer.events.size() != 400) {
        return fail("worker events were not serialized without loss");
    }
    std::array<int, 100> counts{};
    for (const auto& event : observer.events) {
        const auto& message = std::get<MessageEvent>(event);
        ++counts.at(static_cast<std::size_t>(std::stoi(message.text)));
    }
    if (std::ranges::any_of(counts, [](int count) { return count != 4; })) {
        return fail("worker event payloads were lost or duplicated");
    }
    observer.reject = true;
    if (!onedrive::test::throws_with<std::runtime_error>(
            [&observer] { observer.end_download_progress(); },
            "observer rejected event"
        )) {
        return fail("observer exceptions did not reach the caller");
    }
    observer.reject = false;
    observer.end_download_progress();
    if (observer.events.size() != 401) {
        return fail("observer did not recover after a callback exception");
    }
    return EXIT_SUCCESS;
}

int test_headless_sync() {
    using namespace onedrive::test::sync;
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "sync";
    FakeGraphClient graph;
    graph.changes = {file("file", "file.txt", 4)};
    graph.contents["file"] = "data";
    FakeItemStore items;
    FakeMetrics metrics;
    CapturingObserver observer;
    std::filesystem::permissions(
        temporary.path(), std::filesystem::perms::owner_all
    );
    const auto log_file = temporary.path() / "sync.log";
    const onedrive::logging::Session logging{{
        .file = log_file,
        .message_sink = [&](MessageKind kind, std::string_view text) {
            observer.message(kind, "log", text);
        },
    }};
    auto config = config_for(root, false);
    if (onedrive::sync::SyncEngine{config, graph, items, metrics, &observer}
                .synchronize() != onedrive::exit_code::success ||
        onedrive::test::read_file(root / "file.txt") != "data" ||
        !metrics.last_success) {
        return fail("sync did not work with a non-console observer");
    }
    const bool completed =
        std::ranges::any_of(observer.events, [](const Event& event) {
            const auto* progress = std::get_if<DownloadProgressEvent>(&event);
            return progress != nullptr &&
                   progress->state == ProgressState::completed &&
                   progress->completed_files == 1 &&
                   progress->file_count == 1 && progress->downloaded == 4 &&
                   progress->total == 4;
        });
    const auto operation =
        std::ranges::find_if(observer.events, [](const Event& event) {
            return std::holds_alternative<FileOperationEvent>(event);
        });
    if (operation == observer.events.end() ||
        std::get<FileOperationEvent>(*operation).action !=
            FileAction::downloaded ||
        std::get<FileOperationEvent>(*operation).path != "file.txt" ||
        std::get<FileOperationEvent>(*operation).local_path !=
            (root / "file.txt").string()) {
        return fail(
            "completed download did not publish a structured file operation"
        );
    }
    if (!completed) {
        return fail(
            "headless observer did not receive completed sync progress"
        );
    }
    for (const bool preview : {false, true}) {
        for (const bool blocked : {false, true}) {
            observer.events.clear();
            FakeItemStore report_items;
            FakeMetrics report_metrics;
            const auto report_config = config_for(
                temporary.path() / (preview ? "preview" : "apply") /
                    (blocked ? "blocked" : "clear"),
                preview
            );
            graph.changes.clear();
            if (blocked) {
                auto notes = file("notes", "notes.one", 4);
                notes.unsupported_format = "onenote";
                graph.changes.push_back(std::move(notes));
            }
            const auto result =
                onedrive::sync::SyncEngine{
                    report_config,
                    graph,
                    report_items,
                    report_metrics,
                    &observer
                }
                    .synchronize();
            const auto completions =
                std::ranges::count_if(observer.events, [](const Event& event) {
                    const auto* message = std::get_if<MessageEvent>(&event);
                    return message &&
                           message->text.starts_with("Synchronization ") &&
                           message->text.contains("completed");
                });
            const auto blockers =
                std::ranges::count_if(observer.events, [](const Event& event) {
                    return std::holds_alternative<BlockedItemEvent>(event);
                });
            const bool echoed_blocker =
                std::ranges::any_of(observer.events, [](const Event& event) {
                    const auto* message = std::get_if<MessageEvent>(&event);
                    return message && message->event == "log" &&
                           message->text.starts_with("Blocked remote item");
                });
            if (result != (blocked ? onedrive::exit_code::blocked
                                   : onedrive::exit_code::success) ||
                completions != 1 || blockers != (blocked ? 1 : 0) ||
                echoed_blocker ||
                std::ranges::any_of(observer.events, [](const Event& event) {
                    return std::holds_alternative<FileOperationEvent>(event);
                })) {
                return fail(
                    "sync completion or blocker was duplicated or lost"
                );
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_file_operation_actions() {
    using namespace onedrive;
    using namespace onedrive::test::sync;
    for (const auto action :
         {FileAction::downloaded,
          FileAction::uploaded,
          FileAction::moved_locally,
          FileAction::moved_remotely,
          FileAction::deleted_locally,
          FileAction::deleted_remotely}) {
        for (const bool directory : {false, true}) {
            if (directory && (action == FileAction::downloaded ||
                              action == FileAction::uploaded)) {
                continue;
            }
            for (const bool preview : {true, false}) {
                TemporaryDirectory temporary;
                const auto root = temporary.path() / "sync";
                std::filesystem::create_directories(root);
                auto config = config_for(root, false);
                FakeGraphClient graph;
                graph.moved_item_directory = directory;
                FakeItemStore items;
                FakeMetrics metrics;
                CapturingObserver observer;
                const auto synchronize = [&] {
                    return sync::SyncEngine{
                        config, graph, items, metrics, &observer
                    }
                        .synchronize();
                };
                const bool move = action == FileAction::moved_locally ||
                                  action == FileAction::moved_remotely;
                if (action == FileAction::downloaded) {
                    graph.changes = {file("id", "before.txt", 4)};
                    graph.contents["id"] = "data";
                } else {
                    if (directory) {
                        std::filesystem::create_directory(root / "before.txt");
                    } else {
                        test::write_file(root / "before.txt", "data");
                    }
                    if (action != FileAction::uploaded) {
                        items.saved_delta_link = "saved";
                        items.items.emplace(
                            "id",
                            tracked_item(root, "id", "before.txt", directory)
                        );
                    }
                    if (action == FileAction::moved_locally) {
                        auto remote =
                            file("id", "after.txt", directory ? 0 : 4);
                        remote.directory = directory;
                        graph.changes = {std::move(remote)};
                    } else if (action == FileAction::moved_remotely) {
                        config.sync_mode = sync::SyncMode::bidirectional;
                        auto remote =
                            file("id", "before.txt", directory ? 0 : 4);
                        remote.directory = directory;
                        graph.changes = {std::move(remote)};
                        if (synchronize() != exit_code::success) {
                            return fail(
                                "could not prepare local-rename history fixture"
                            );
                        }
                        observer.events.clear();
                        graph.changes.clear();
                        std::filesystem::rename(
                            root / "before.txt", root / "after.txt"
                        );
                    } else if (action == FileAction::deleted_locally) {
                        graph.changes = {deleted_item("id")};
                    } else {
                        config.sync_mode = sync::SyncMode::upload_only;
                        if (action == FileAction::deleted_remotely) {
                            std::filesystem::remove(root / "before.txt");
                        }
                    }
                }
                config.dry_run = preview;
                const auto started = std::chrono::system_clock::now();
                if (synchronize() != exit_code::success) {
                    return fail(
                        "file-operation history fixture did not synchronize"
                    );
                }
                std::vector<FileOperationEvent> operations;
                for (const auto& event : observer.events) {
                    if (const auto* operation =
                            std::get_if<FileOperationEvent>(&event)) {
                        operations.push_back(*operation);
                    }
                }
                if (operations.size() != (preview ? 0U : 1U)) {
                    return fail(
                        "preview invented history or a completed action was "
                        "duplicated/lost"
                    );
                }
                if (!preview) {
                    const auto& operation = operations.front();
                    const auto expected = move ? "after.txt" : "before.txt";
                    if (operation.action != action ||
                        operation.path != expected ||
                        operation.previous_path != (move ? "before.txt" : "") ||
                        operation.local_path != (root / expected).string() ||
                        operation.drive_id != "me" ||
                        operation.directory != directory ||
                        operation.recovered ||
                        operation.completed_at < started ||
                        operation.completed_at >
                            std::chrono::system_clock::now()) {
                        return fail(
                            "file-operation action, paths or completion time "
                            "were "
                            "incorrect"
                        );
                    }
                }
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_recovered_file_operations() {
    using namespace onedrive;
    using namespace onedrive::test::sync;
    for (const auto action :
         {FileAction::uploaded,
          FileAction::moved_locally,
          FileAction::moved_remotely,
          FileAction::deleted_remotely}) {
        TemporaryDirectory temporary;
        const auto root = temporary.path() / "sync";
        std::filesystem::create_directories(root);
        const auto before = root / "before.txt";
        const auto after = root / "after.txt";
        test::write_file(before, "data");
        auto config = config_for(root, false);
        FakeGraphClient graph;
        FakeItemStore items;
        FakeMetrics metrics;
        CapturingObserver observer;
        const auto synchronize = [&] {
            return sync::SyncEngine{config, graph, items, metrics, &observer}
                .synchronize();
        };
        items.saved_delta_link = "saved";
        if (action != FileAction::uploaded) {
            items.items.emplace("id", tracked_item(root, "id", "before.txt"));
        }
        const bool moving = action == FileAction::moved_locally ||
                            action == FileAction::moved_remotely;
        if (action == FileAction::moved_locally) {
            struct stat identity{};
            if (::stat(before.c_str(), &identity) != 0) {
                return fail("could not inspect local recovery fixture");
            }
            items.save_pending_move({
                .drive_id = "me",
                .remote_id = "id",
                .source_path = before,
                .destination_path = after,
                .source_device = static_cast<std::uint64_t>(identity.st_dev),
                .source_inode = static_cast<std::uint64_t>(identity.st_ino),
            });
            graph.changes = {file("id", "after.txt", 4)};
            config.dry_run = true;
            if (synchronize() != exit_code::success ||
                !observer.file_operations().empty() ||
                items.pending_moves_by_id.size() != 1 ||
                !std::filesystem::exists(before)) {
                return fail(
                    "preview executed or recorded a pending local move"
                );
            }
            config.dry_run = false;
        } else {
            config.sync_mode = sync::SyncMode::upload_only;
            std::string expected_error;
            if (action == FileAction::uploaded) {
                config.sync_mode = sync::SyncMode::bidirectional;
                items.fail_commit_upload = true;
                expected_error = "simulated upload commit failure";
            } else if (action == FileAction::deleted_remotely) {
                std::filesystem::remove(before);
                items.fail_commit_delete = true;
                expected_error = "simulated deletion commit failure";
            } else {
                config.sync_mode = sync::SyncMode::bidirectional;
                if (synchronize() != exit_code::success) {
                    return fail("could not prepare pending remote move");
                }
                observer.events.clear();
                std::filesystem::rename(before, after);
                items.fail_commit_remote_move = true;
                expected_error = "simulated remote move commit failure";
            }
            if (!test::throws_with<std::runtime_error>(
                    [&] { static_cast<void>(synchronize()); }, expected_error
                ) ||
                !observer.file_operations().empty()) {
                return fail(
                    "an uncommitted operation was reported as completed"
                );
            }
            items.fail_commit_upload = false;
            items.fail_commit_delete = false;
            items.fail_commit_remote_move = false;
            if (action == FileAction::uploaded) {
                graph.upload_conflict = true;
                graph.lookup_item = file("id", "before.txt", 4);
                graph.contents["id"] = "data";
            } else if (moving) {
                graph.move_conflict = true;
                graph.lookup_item = file("id", "after.txt", 4);
            }
        }
        observer.events.clear();
        const auto started = std::chrono::system_clock::now();
        if (synchronize() != exit_code::success) {
            return fail("a pending file operation could not be recovered");
        }
        const auto operations = observer.file_operations();
        const auto path = moving ? "after.txt" : "before.txt";
        if (operations.size() != 1 || operations.front().action != action ||
            !operations.front().recovered || operations.front().directory ||
            operations.front().path != path ||
            operations.front().previous_path != (moving ? "before.txt" : "") ||
            operations.front().drive_id != "me" ||
            operations.front().local_path != (root / path).string() ||
            operations.front().completed_at < started ||
            !items.pending_moves_by_id.empty() ||
            !items.pending_remote_moves_by_id.empty() ||
            !items.pending_uploads_by_path.empty() ||
            !items.pending_deletes_by_id.empty()) {
            return fail(
                "recovery lost its completion metadata or retained a journal"
            );
        }
        graph.changes.clear();
        observer.events.clear();
        if (synchronize() != exit_code::success ||
            !observer.file_operations().empty()) {
            return fail("a subsequent round recorded the same recovery again");
        }
    }
    return EXIT_SUCCESS;
}

int test_staged_move_operation_history() {
    using namespace onedrive;
    using namespace onedrive::test::sync;
    for (const bool recovering : {false, true}) {
        TemporaryDirectory temporary;
        const auto root = temporary.path() / "sync";
        std::filesystem::create_directories(root);
        test::write_file(root / "A.txt", "AAAA");
        test::write_file(root / "B.txt", "BBBB");
        FakeItemStore items;
        items.saved_delta_link = "saved";
        items.items.emplace("a", tracked_item(root, "a", "A.txt"));
        items.items.emplace("b", tracked_item(root, "b", "B.txt"));
        if (recovering) {
            struct stat identity{};
            if (::stat((root / "A.txt").c_str(), &identity) != 0) {
                return fail("could not inspect staged move fixture");
            }
            const auto staging = root / ".A.txt.onedrive-move-recovery";
            items.save_pending_move({
                .drive_id = "me",
                .remote_id = "a",
                .source_path = root / "A.txt",
                .destination_path = root / "B.txt",
                .staging_path = staging,
                .source_device = static_cast<std::uint64_t>(identity.st_dev),
                .source_inode = static_cast<std::uint64_t>(identity.st_ino),
            });
            std::filesystem::rename(root / "A.txt", staging);
        }
        FakeGraphClient graph;
        graph.changes = {file("a", "B.txt", 4), file("b", "A.txt", 4)};
        FakeMetrics metrics;
        CapturingObserver observer;
        if (sync::SyncEngine{
                config_for(root, false), graph, items, metrics, &observer
            }
                    .synchronize() != exit_code::success ||
            test::read_file(root / "A.txt") != "BBBB" ||
            test::read_file(root / "B.txt") != "AAAA" ||
            !items.pending_moves_by_id.empty()) {
            return fail("a name exchange lost its files or recovery journal");
        }
        const auto operations = observer.file_operations();
        if (operations.size() != 2) {
            return fail(
                "a staged exchange lost a final move or recorded internal "
                "staging"
            );
        }
        for (const auto& operation : operations) {
            const bool first = operation.previous_path == "A.txt";
            const auto expected = first ? "B.txt" : "A.txt";
            if ((!first && operation.previous_path != "B.txt") ||
                operation.action != FileAction::moved_locally ||
                operation.path != expected ||
                operation.local_path != (root / expected).string() ||
                operation.recovered != (recovering && first)) {
                return fail(
                    "staging leaked into history or a new journal was marked "
                    "as recovered"
                );
            }
        }
        if (operations[0].path == operations[1].path) {
            return fail("a name exchange recorded the same final move twice");
        }
    }
    return EXIT_SUCCESS;
}

int test_partial_file_operation_history() {
    using namespace onedrive;
    using namespace onedrive::test::sync;
    for (const bool uploading : {false, true}) {
        TemporaryDirectory temporary;
        const auto root = temporary.path() / "sync";
        std::filesystem::create_directories(root);
        auto config = config_for(root, false);
        config.transfer_order = config::TransferOrder::name_ascending;
        config.download_concurrency = 1;
        config.upload_concurrency = 1;
        FakeGraphClient graph;
        FakeItemStore items;
        FakeMetrics metrics;
        CapturingObserver observer;
        if (uploading) {
            config.sync_mode = sync::SyncMode::upload_only;
            test::write_file(root / "a-good.txt", "data");
            test::write_file(root / "z-bad.txt", "data");
            graph.upload_resource_error_path = "z-bad.txt";
        } else {
            graph.changes = {
                file("good", "a-good.txt", 4), file("bad", "z-bad.txt", 4)
            };
            graph.contents["good"] = "data";
            graph.failing_id = "bad";
        }
        const auto synchronize = [&] {
            return sync::SyncEngine{config, graph, items, metrics, &observer}
                .synchronize();
        };
        if (uploading) {
            if (synchronize() != exit_code::blocked) {
                return fail(
                    "upload failure fixture did not block its failed operation"
                );
            }
        } else if (!test::throws_with<std::runtime_error>(
                       [&] { static_cast<void>(synchronize()); },
                       "simulated download failure"
                   )) {
            return fail("download failure fixture did not surface its error");
        }
        std::vector<FileOperationEvent> operations;
        for (const auto& event : observer.events) {
            if (const auto* operation =
                    std::get_if<FileOperationEvent>(&event)) {
                operations.push_back(*operation);
            }
        }
        if (operations.size() != 1 || operations.front().path != "a-good.txt" ||
            operations.front().action !=
                (uploading ? FileAction::uploaded : FileAction::downloaded)) {
            return fail(
                "a failed round lost completed work or recorded an "
                "unsuccessful transfer"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_file_history() {
    if (!onedrive::test::throws_with<std::invalid_argument>(
            [] { const FileHistory invalid{0}; }, "must be positive"
        ) ||
        format_file_operation_time(
            std::chrono::system_clock::time_point{std::chrono::milliseconds{-1}}
        ) != "1969-12-31T23:59:59Z" ||
        format_file_operation_time(
            std::chrono::system_clock::time_point{
                std::chrono::milliseconds{999}
            }
        ) != "1970-01-01T00:00:00Z") {
        return fail(
            "history accepted a zero limit or rounded fractional completion "
            "times"
        );
    }
    FileHistory history{10'000};
    for (int index = 0; index <= 10'000; ++index) {
        history.add({.path = std::to_string(index)});
    }
    if (history.entries().size() != 10'000 || history.omitted() != 1 ||
        history.entries().front().path != "1" ||
        history.entries().back().path != "10000") {
        return fail(
            "file history did not enforce the exact 10000-entry boundary"
        );
    }
    history.set_limit(3);
    if (history.entries().size() != 3 || history.omitted() != 9998 ||
        history.entries().front().path != "9998") {
        return fail("shrinking history lost omission accounting");
    }
    history.set_limit(5);
    history.add({.path = "same.txt"});
    history.add({.path = "same.txt"});
    if (history.entries().size() != 5 || history.omitted() != 9998 ||
        history.entries()[3].path != history.entries()[4].path ||
        !onedrive::test::throws_with<std::invalid_argument>(
            [&] { history.set_limit(0); }, "must be positive"
        )) {
        return fail(
            "history discarded repeated actions or accepted an invalid limit"
        );
    }
    CapturingObserver observer;
    FileOperationEvent operation{
        .completed_at = std::chrono::system_clock::time_point{},
        .action = FileAction::moved_remotely,
        .path = "after.txt",
        .previous_path = "before.txt",
        .drive_id = "drive",
        .directory = true,
        .recovered = true,
    };
    observer.file_operation(operation);
    operation.path = "changed.txt";
    const auto& captured =
        std::get<FileOperationEvent>(observer.events.front());
    if (captured.path != "after.txt" ||
        captured.previous_path != "before.txt" ||
        captured.drive_id != "drive" || !captured.directory ||
        !captured.recovered ||
        file_action_name(captured.action) != "moved_remotely" ||
        format_file_operation_time(captured.completed_at) !=
            "1970-01-01T00:00:00Z") {
        return fail(
            "file operation borrowed its payload or changed its timestamp"
        );
    }
    history.clear();
    return history.entries().empty() && history.omitted() == 0
               ? EXIT_SUCCESS
               : fail("history clear did not reset omission accounting");
}

int test_sync_summary_accumulation() {
    SyncSummaryEvent totals;
    const std::array rounds{
        SyncSummaryEvent{
            .downloaded = 1,
            .uploaded = 2,
            .moved = 3,
            .deleted = 4,
            .blocked = 47
        },
        SyncSummaryEvent{.blocked = 47},
        SyncSummaryEvent{
            .downloaded = 5,
            .uploaded = 6,
            .moved = 7,
            .deleted = 8,
            .blocked = 3
        },
        SyncSummaryEvent{},
    };
    for (const auto& round : rounds) {
        const auto previous = totals;
        totals = accumulate_sync_summary(totals, round);
        if (totals.downloaded != previous.downloaded + round.downloaded ||
            totals.uploaded != previous.uploaded + round.uploaded ||
            totals.moved != previous.moved + round.moved ||
            totals.deleted != previous.deleted + round.deleted ||
            totals.blocked != round.blocked) {
            return fail(
                "shared summary rules lost counts or accumulated blockers"
            );
        }
    }
    if (totals.downloaded != 6 || totals.uploaded != 8 || totals.moved != 10 ||
        totals.deleted != 12 || totals.blocked != 0) {
        return fail(
            "an empty round reset accumulated operations or stale blockers"
        );
    }
    const auto restarted = accumulate_sync_summary({}, rounds.front());
    if (restarted.downloaded != 1 || restarted.uploaded != 2 ||
        restarted.moved != 3 || restarted.deleted != 4 ||
        restarted.blocked != 47) {
        return fail("shared summary rules did not support a fresh session");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (test_payloads() != EXIT_SUCCESS ||
        test_workers_and_errors() != EXIT_SUCCESS ||
        test_sync_summary_accumulation() != EXIT_SUCCESS ||
        test_file_history() != EXIT_SUCCESS ||
        test_file_operation_actions() != EXIT_SUCCESS ||
        test_recovered_file_operations() != EXIT_SUCCESS ||
        test_staged_move_operation_history() != EXIT_SUCCESS ||
        test_partial_file_operation_history() != EXIT_SUCCESS ||
        test_headless_sync() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
