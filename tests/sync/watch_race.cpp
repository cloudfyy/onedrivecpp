#include "support/progress.hpp"
#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "sync/filesystem/operations.hpp"
#include "sync/upload/remote_delete.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace onedrive::test::sync;
using namespace std::chrono_literals;

std::atomic_bool remove_during_identity{false};
std::atomic_int identities_before_removal{0};

std::filesystem::path snapshot_source;
std::function<void()> mutate_snapshot_source;
std::atomic_bool intercept_snapshot_read{false};
std::atomic_int reads_before_mutation{0};
bool snapshot_read_failure{false};

bool prepare_snapshot_read(int descriptor) {
    if (intercept_snapshot_read.load()) {
        std::array<char, 4096> path{};
        const auto count = ::readlink(
            ("/proc/self/fd/" + std::to_string(descriptor)).c_str(),
            path.data(),
            path.size()
        );
        if (count > 0 &&
            std::string_view{path.data(), static_cast<std::size_t>(count)} ==
                snapshot_source.native() &&
            reads_before_mutation.fetch_sub(1) == 0 &&
            intercept_snapshot_read.exchange(false)) {
            if (snapshot_read_failure) {
                errno = EIO;
                return false;
            }
            mutate_snapshot_source();
        }
    }
    return true;
}

struct SnapshotReadGuard {
    ~SnapshotReadGuard() {
        intercept_snapshot_read = false;
        mutate_snapshot_source = {};
    }
};

class UploadRaceObserver final : public onedrive::events::Observer {
public:
    mutable std::vector<onedrive::events::Event> events;

private:
    void on_event(const onedrive::events::Event& event) const override {
        events.push_back(event);
    }
};

enum class SnapshotMutation {
    size,
    content,
    replacement,
    deletion,
    read_failure
};

int test_edit_during_snapshot(
    bool tracked, bool upload_only, SnapshotMutation mutation
) {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    const auto source = root / "changing.txt";
    onedrive::test::write_file(source, "data");
    FakeItemStore items;
    items.saved_delta_link = "saved";
    const auto previous = tracked_item(root, "file", "changing.txt");
    if (tracked) {
        items.items.emplace("file", previous);
    }
    const std::string initial = mutation == SnapshotMutation::size
                                    ? std::string(131089, 'A')
                                    : "initial";
    onedrive::test::write_file(source, initial);
    const auto initial_time = std::filesystem::last_write_time(source);
    onedrive::test::write_file(root / "ready.txt", "ready");
    FakeGraphClient graph;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = upload_only ? onedrive::sync::SyncMode::upload_only
                                   : onedrive::sync::SyncMode::bidirectional;
    config.upload_concurrency = 2;
    UploadRaceObserver observer;
    const auto synchronize = [&] {
        return onedrive::sync::SyncEngine{
            config, graph, items, metrics, &observer
        }
            .synchronize();
    };
    const SnapshotReadGuard read_guard;
    snapshot_source = source;
    reads_before_mutation = mutation == SnapshotMutation::size ? 1 : 0;
    snapshot_read_failure = mutation == SnapshotMutation::read_failure;
    const std::string edited =
        mutation == SnapshotMutation::size ? "local-new-content" : "CHANGED";
    mutate_snapshot_source = [&] {
        if (mutation == SnapshotMutation::deletion) {
            std::filesystem::remove(source);
        } else if (mutation == SnapshotMutation::replacement) {
            const auto replacement = root / ".atomic-save";
            onedrive::test::write_file(replacement, edited);
            std::filesystem::last_write_time(replacement, initial_time);
            std::filesystem::rename(replacement, source);
        } else {
            onedrive::test::write_file(source, edited);
            std::filesystem::last_write_time(source, initial_time);
        }
    };
    intercept_snapshot_read = true;
    int result = -1;
    try {
        result = synchronize();
    } catch (const std::exception& error) {
        return fail(
            "snapshot preparation interrupted synchronization: " +
            std::string{error.what()}
        );
    }
    const bool read_failure = mutation == SnapshotMutation::read_failure;
    const bool deleted = mutation == SnapshotMutation::deletion;
    const auto warning_code = read_failure ? "local_upload_resource_blocked"
                                           : "local_upload_source_changed";
    if (intercept_snapshot_read.exchange(false) ||
        result != (read_failure ? onedrive::exit_code::blocked
                                : onedrive::exit_code::success) ||
        graph.upload_count != 1 ||
        graph.uploaded_paths != std::vector<std::string>{"ready.txt"} ||
        !graph.deleted_items.empty() ||
        items.blocked.size() != (read_failure ? 1U : 0U) ||
        items.pending_uploads_by_path.size() != (read_failure ? 1U : 0U) ||
        std::ranges::none_of(
            observer.events,
            [&](const auto& event) {
                const auto* message =
                    std::get_if<onedrive::events::MessageEvent>(&event);
                return message && message->event == warning_code &&
                       message->kind ==
                           onedrive::events::MessageKind::warning &&
                       message->text.contains("changing.txt");
            }
        ) ||
        std::ranges::none_of(
            observer.events,
            [&](const auto& event) {
                const auto* summary =
                    std::get_if<onedrive::events::SyncSummaryEvent>(&event);
                return summary && summary->uploaded == 1 &&
                       summary->blocked == (read_failure ? 1U : 0U);
            }
        ) ||
        (!read_failure &&
         std::ranges::none_of(
             observer.events,
             [](const auto& event) {
                 const auto* progress =
                     std::get_if<onedrive::events::UploadProgressEvent>(&event);
                 return progress &&
                        progress->state ==
                            onedrive::util::ProgressState::completed &&
                        progress->completed_files == 1 &&
                        progress->file_count == 1 && progress->uploaded == 5 &&
                        progress->total == 5;
             }
         )) ||
        std::ranges::any_of(
            observer.events,
            [](const auto& event) {
                const auto* operation =
                    std::get_if<onedrive::events::FileOperationEvent>(&event);
                return operation && operation->path == "changing.txt";
            }
        ) ||
        std::ranges::any_of(
            std::filesystem::directory_iterator{root}, [](const auto& entry) {
                return entry.path().filename().string().contains(
                    ".onedrive-upload-"
                );
            }
        )) {
        return fail(
            "unstable snapshot was uploaded, hidden, or retained as a journal"
        );
    }
    const auto retained = items.find("me", "file");
    if (tracked &&
        (!retained || retained->local_size != previous.local_size ||
         retained->local_modified_ticks != previous.local_modified_ticks)) {
        return fail(
            "unstable snapshot replaced the trusted synchronized baseline"
        );
    }
    if (read_failure) {
        const auto& pending = items.pending_uploads_by_path.begin()->second;
        if (!pending.snapshot_path.empty() ||
            pending.failure_code != "local_read" ||
            items.blocked.front().reason_code != "upload_local_read") {
            return fail(
                "genuine snapshot read failure lost its resource diagnostic"
            );
        }
    }
    if (deleted) {
        if (std::filesystem::exists(source)) {
            return fail("snapshot preparation restored a deleted source");
        }
    } else if (onedrive::test::read_file(source) !=
               (read_failure ? initial : edited)) {
        return fail("snapshot preparation discarded the latest local content");
    }
    observer.events.clear();
    std::string uploaded;
    graph.during_upload = [&] {
        const auto pending = items.pending_uploads("me");
        if (pending.size() != 1) {
            throw std::runtime_error{"expected one stable upload snapshot"};
        }
        uploaded = onedrive::test::read_file(pending.front().snapshot_path);
    };
    if (synchronize() != onedrive::exit_code::success ||
        graph.upload_count != (deleted ? 1 : 2) ||
        graph.deleted_items.size() != (deleted && tracked ? 1U : 0U) ||
        !items.blocked.empty() || !items.pending_uploads_by_path.empty() ||
        (!deleted && uploaded != (read_failure ? initial : edited))) {
        return fail(
            "a stable follow-up round did not reconcile the deferred source"
        );
    }
    return EXIT_SUCCESS;
}

int test_watch_retries_unstable_snapshot() {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    const auto source = root / "changing.txt";
    onedrive::test::write_file(source, "initial");
    FakeItemStore items;
    items.saved_delta_link = "saved";
    FakeGraphClient graph;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    UploadRaceObserver observer;
    const SnapshotReadGuard read_guard;
    snapshot_source = source;
    reads_before_mutation = 0;
    snapshot_read_failure = false;
    mutate_snapshot_source = [&] {
        onedrive::test::write_file(source, "local-new-content");
    };
    intercept_snapshot_read = true;
    std::atomic_int rounds{0};
    bool deferred = false;
    std::atomic_bool reconciled{false};
    onedrive::monitor::Monitor monitor{
        root,
        [&](const std::stop_token& stop) {
            const auto result =
                onedrive::sync::SyncEngine{
                    config, graph, items, metrics, &observer
                }
                    .synchronize(stop);
            if (++rounds == 1) {
                deferred =
                    result == onedrive::exit_code::success &&
                    graph.upload_count == 0 &&
                    std::ranges::any_of(observer.events, [](const auto& event) {
                        const auto* message =
                            std::get_if<onedrive::events::MessageEvent>(&event);
                        return message &&
                               message->event == "local_upload_source_changed";
                    });
            } else {
                reconciled = result == onedrive::exit_code::success &&
                             graph.upload_count == 1;
            }
            return result;
        },
        1h,
        5ms,
    };
    int result = -1;
    std::exception_ptr error;
    std::jthread worker{[&](std::stop_token stop) {
        try {
            result = monitor.run(stop);
        } catch (...) {
            error = std::current_exception();
        }
    }};
    const bool uploaded =
        onedrive::test::wait_until([&] { return reconciled.load(); }, 3s, 5ms);
    worker.request_stop();
    worker.join();
    if (error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& failure) {
            return fail(
                "watch exited during snapshot creation: " +
                std::string{failure.what()}
            );
        }
    }
    const auto current = items.find("me", "uploaded-1");
    if (!uploaded || !deferred || rounds < 2 ||
        result != onedrive::exit_code::success || graph.upload_count != 1 ||
        !current || current->local_size != 17 ||
        onedrive::test::read_file(source) != "local-new-content" ||
        !items.blocked.empty() || !items.pending_uploads_by_path.empty()) {
        return fail("watch did not automatically retry the stable local edit");
    }
    return EXIT_SUCCESS;
}

int test_journaled_modification_is_not_skipped() {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    onedrive::test::write_file(root / "file.txt", "data");
    FakeItemStore items;
    items.saved_delta_link = "saved";
    FakeGraphClient graph;
    graph.during_upload = [] {
        throw onedrive::sync::detail::LocalModificationConflictError{
            "simulated journaled upload conflict"
        };
    };
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    UploadRaceObserver observer;
    const auto synchronize = [&] {
        return onedrive::sync::SyncEngine{
            config, graph, items, metrics, &observer
        }
            .synchronize();
    };
    bool failed = false;
    try {
        static_cast<void>(synchronize());
    } catch (const onedrive::sync::detail::LocalModificationConflictError&) {
        failed = true;
    }
    if (!failed || graph.upload_count != 1 ||
        items.pending_uploads_by_path.size() != 1 ||
        items.blocked.size() != 1 ||
        items.blocked.front().reason_code != "upload_local_modification" ||
        onedrive::test::read_file(
            items.pending_uploads_by_path.begin()->second.snapshot_path
        ) != "data" ||
        metrics.last_success) {
        return fail(
            "journaled upload conflict was incorrectly treated as a skip"
        );
    }
    if (synchronize() != onedrive::exit_code::success ||
        graph.upload_count != 2 || !items.pending_uploads_by_path.empty() ||
        !items.blocked.empty() || !items.find("me", "uploaded-2")) {
        return fail(
            "journaled upload conflict lost its existing recovery path"
        );
    }
    return EXIT_SUCCESS;
}

class ReuseMutationSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::function<void()> before_reuse;

private:
    void sink_it_(const spdlog::details::log_msg& message) override {
        const std::string_view text{
            message.payload.data(), message.payload.size()
        };
        if (before_reuse &&
            text.starts_with("Reusing completed download for")) {
            auto callback = std::move(before_reuse);
            before_reuse = {};
            callback();
        }
    }
    void flush_() override {
    }
};

struct DefaultLoggerGuard {
    std::shared_ptr<spdlog::logger> previous{spdlog::default_logger()};

    ~DefaultLoggerGuard() {
        spdlog::set_default_logger(previous);
    }
};

enum class ReuseMutation { size, modified_time, atomic_save, none };

int test_edit_during_delta_reuse(
    bool download_only, bool metadata_echo, ReuseMutation mutation
) {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    const auto destination = root / "file.txt";
    onedrive::test::write_file(destination, "data");
    const auto original_time = std::filesystem::last_write_time(destination);
    auto previous = tracked_item(root, "file", "file.txt");
    previous.ctag = metadata_echo ? "content-v1" : "";
    FakeItemStore items;
    items.saved_delta_link = "saved";
    items.items.emplace("file", previous);
    FakeGraphClient graph;
    auto remote = file("file", "file.txt", 4);
    remote.ctag = previous.ctag;
    if (metadata_echo) {
        remote.etag = "metadata-v2";
    }
    graph.changes = {remote};
    graph.contents["file"] = "data";
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = download_only ? onedrive::sync::SyncMode::download_only
                                     : onedrive::sync::SyncMode::bidirectional;
    const onedrive::cli::Console console{{.quiet = true}};
    const auto synchronize = [&] {
        return onedrive::sync::SyncEngine{
            config, graph, items, metrics, &console
        }
            .synchronize();
    };
    const DefaultLoggerGuard logger_guard;
    const auto sink = std::make_shared<ReuseMutationSink>();
    auto logger = std::make_shared<spdlog::logger>("reuse-race", sink);
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(std::move(logger));
    unsigned injected = 0;
    const std::string expected = mutation == ReuseMutation::size   ? "local-new"
                                 : mutation == ReuseMutation::none ? "data"
                                                                   : "EDIT";
    sink->before_reuse = [&] {
        ++injected;
        if (mutation == ReuseMutation::none) {
            return;
        }
        if (mutation == ReuseMutation::atomic_save) {
            const auto edited = root / ".atomic-save";
            onedrive::test::write_file(edited, expected);
            std::filesystem::last_write_time(edited, original_time + 1s);
            std::filesystem::rename(edited, destination);
        } else {
            onedrive::test::write_file(destination, expected);
            std::filesystem::last_write_time(
                destination,
                mutation == ReuseMutation::size ? original_time
                                                : original_time + 1s
            );
        }
    };
    std::string uploaded;
    graph.during_upload = [&] {
        if (items.pending_uploads_by_path.size() != 1) {
            throw std::runtime_error{"expected one journaled upload snapshot"};
        }
        uploaded = onedrive::test::read_file(
            items.pending_uploads_by_path.begin()->second.snapshot_path
        );
    };
    const bool should_upload =
        !download_only && mutation != ReuseMutation::none;
    const auto result = synchronize();
    const auto saved = std::ranges::find(
        items.applied_delta.upserts,
        "file",
        &onedrive::storage::ItemState::remote_id
    );
    if (result != onedrive::exit_code::success || injected != 1 ||
        saved == items.applied_delta.upserts.end() ||
        saved->local_size != previous.local_size ||
        saved->local_modified_ticks != previous.local_modified_ticks ||
        saved->etag != remote.etag || saved->ctag != remote.ctag ||
        saved->local_device == 0 || saved->local_inode == 0 ||
        graph.download_count != 0 ||
        graph.upload_count != (should_upload ? 1 : 0) ||
        (should_upload && uploaded != expected) ||
        onedrive::test::read_file(destination) != expected ||
        !items.blocked.empty() || !items.pending_uploads_by_path.empty()) {
        return fail(
            "download reuse absorbed a later edit into the synchronized "
            "baseline "
            "(download-only " +
            std::to_string(download_only) + ", metadata echo " +
            std::to_string(metadata_echo) + ", mutation " +
            std::to_string(static_cast<unsigned>(mutation)) + ")"
        );
    }
    graph.changes.clear();
    const auto expected_size = should_upload
                                   ? static_cast<std::int64_t>(expected.size())
                                   : previous.local_size;
    const auto expected_ticks =
        should_upload ? onedrive::sync::detail::modified_ticks(destination)
                      : previous.local_modified_ticks;
    if (synchronize() != onedrive::exit_code::success ||
        graph.upload_count != (should_upload ? 1 : 0) ||
        graph.download_count != 0 ||
        onedrive::test::read_file(destination) != expected ||
        !items.blocked.empty()) {
        return fail(
            "a subsequent round repeated or discarded the reused-file edit"
        );
    }
    const auto current = items.find("me", "file");
    if (!current || current->local_size != expected_size ||
        current->local_modified_ticks != expected_ticks) {
        return fail(
            "the persisted snapshot no longer matches its trusted baseline"
        );
    }
    return EXIT_SUCCESS;
}

int test_delete_during_watch_scan(int identities) {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "sync";
    std::filesystem::create_directory(root);
    std::ofstream{root / "copied.txt"} << "data";
    FakeItemStore items;
    items.saved_delta_link = "saved";
    FakeGraphClient graph;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    const onedrive::cli::Console console;
    std::atomic_int rounds{0};
    std::atomic_bool deleted{false};
    std::atomic_bool deferred{false};
    onedrive::monitor::Monitor monitor{
        root,
        [&](const std::stop_token& stop) {
            const auto result =
                onedrive::sync::SyncEngine{
                    config, graph, items, metrics, &console
                }
                    .synchronize(stop);
            if (++rounds == 1) {
                identities_before_removal = identities;
                remove_during_identity = true;
                if (::chmod((root / "copied.txt").c_str(), 0600) != 0) {
                    throw std::runtime_error{"cannot trigger local scan"};
                }
            } else {
                deferred = deferred || result == onedrive::exit_code::failure;
                deleted = !graph.deleted_items.empty();
            }
            return result;
        },
        1h,
        5ms,
    };
    std::exception_ptr error;
    int result = -1;
    std::jthread worker{[&](std::stop_token stop) {
        try {
            result = monitor.run(stop);
        } catch (...) {
            error = std::current_exception();
        }
    }};
    const bool reconciled =
        onedrive::test::wait_until([&] { return deleted.load(); }, 3s, 5ms);
    worker.request_stop();
    worker.join();
    remove_during_identity = false;
    if (error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& failure) {
            return fail(
                "watch exited when an uploaded file disappeared during scan: " +
                std::string{failure.what()}
            );
        }
    }
    if (!reconciled || !deferred || rounds < 3 ||
        result != onedrive::exit_code::success || graph.upload_count != 1 ||
        graph.download_count != 0 || graph.deleted_items.size() != 1 ||
        std::filesystem::exists(root / "copied.txt") || !items.items.empty() ||
        !items.blocked.empty()) {
        return fail(
            "watch did not reconcile deletion after an interrupted scan"
        );
    }
    return EXIT_SUCCESS;
}

int test_delete_during_delta_reuse() {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    std::ofstream{root / "copied.txt"} << "data";
    FakeItemStore items;
    items.saved_delta_link = "saved";
    items.items.emplace("file", tracked_item(root, "file", "copied.txt"));
    FakeGraphClient graph;
    graph.delta_override = [&](const auto&) {
        identities_before_removal = 0;
        remove_during_identity = true;
        return onedrive::graph::DeltaResult{
            .changes = {file("file", "copied.txt", 4)},
            .delta_link = "next",
        };
    };
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    const onedrive::cli::Console console;
    const auto result =
        onedrive::sync::SyncEngine{config, graph, items, metrics, &console}
            .synchronize();
    if (remove_during_identity.exchange(false) ||
        result != onedrive::exit_code::success || graph.download_count != 0 ||
        graph.upload_count != 0 || graph.deleted_items.size() != 1 ||
        !items.items.empty() || !items.blocked.empty()) {
        return fail("Delta reuse resurrected a concurrently deleted file");
    }
    return EXIT_SUCCESS;
}

int test_delete_before_upload_commit() {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    std::ofstream{root / "copied.txt"} << "data";
    FakeItemStore items;
    items.saved_delta_link = "saved";
    FakeGraphClient graph;
    graph.before_upload_return = [] {
        identities_before_removal = 0;
        remove_during_identity = true;
    };
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    const onedrive::cli::Console console;
    const auto synchronize = [&] {
        return onedrive::sync::SyncEngine{
            config, graph, items, metrics, &console
        }
            .synchronize();
    };
    if (synchronize() != onedrive::exit_code::success ||
        remove_during_identity.exchange(false) || items.items.size() != 1 ||
        !items.pending_uploads_by_path.empty() ||
        std::filesystem::exists(root / "copied.txt") ||
        synchronize() != onedrive::exit_code::success ||
        graph.download_count != 0 || graph.upload_count != 1 ||
        graph.deleted_items.size() != 1 || !items.items.empty() ||
        !items.blocked.empty()) {
        return fail("upload commit lost a concurrently deleted source");
    }
    return EXIT_SUCCESS;
}

int test_delete_metadata_version_race() {
    for (const int scenario : {0, 1, 2, 3, 4}) {
        TemporaryDirectory temporary;
        const auto root = temporary.path();
        std::ofstream{root / "copied.txt"} << "data";
        FakeItemStore items;
        auto previous = tracked_item(root, "file", "copied.txt");
        previous.ctag = "content-v1";
        items.items.emplace("file", previous);
        std::filesystem::remove(root / "copied.txt");
        FakeGraphClient graph;
        auto current = file("file", "copied.txt", 4);
        current.etag = "metadata-v2";
        current.ctag = scenario == 1 ? "content-v2" : "content-v1";
        if (scenario == 2) {
            current.id = "replacement";
        }
        graph.lookup_item = current;
        graph.delete_conflict = true;
        graph.during_remote_delete = [&] {
            if (scenario == 4) {
                std::ofstream{root / "copied.txt"} << "keep";
            }
            graph.during_remote_delete = [&] {
                graph.delete_conflict = scenario == 3;
            };
        };
        onedrive::graph::GraphClient graph_port{
            onedrive::util::borrowed_proxy, graph
        };
        onedrive::storage::ItemStore store{
            onedrive::util::borrowed_proxy, items
        };
        bool conflict = false;
        try {
            onedrive::sync::detail::execute_new_remote_delete(
                {
                    .drive_id = "me",
                    .remote_id = "file",
                    .expected_etag = previous.etag,
                    .remote_path = previous.remote_path,
                    .local_path = previous.local_path,
                },
                graph_port,
                store
            );
        } catch (
            const onedrive::sync::detail::LocalModificationConflictError&
        ) {
            conflict = true;
        }
        const bool retry = scenario == 0 || scenario == 3;
        if (conflict != (scenario != 0) ||
            graph.deleted_items.size() != (retry ? 2U : 1U) ||
            (retry && graph.deleted_items.back().second != "metadata-v2") ||
            items.items.empty() != (scenario == 0) ||
            !items.pending_deletes_by_id.empty() ||
            items.blocked.empty() != (scenario == 0) ||
            (scenario == 4 &&
             onedrive::test::read_file(root / "copied.txt") != "keep")) {
            return fail(
                "metadata-only deletion retry bypassed conflict safety"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_create_delete_during_sync() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "sync";
    std::filesystem::create_directory(root);
    std::atomic_int rounds{0};
    onedrive::monitor::Monitor monitor{
        root,
        [&](const std::stop_token&) {
            if (++rounds == 1) {
                std::ofstream{root / "copied.txt"} << "data";
                std::filesystem::remove(root / "copied.txt");
            }
            return onedrive::exit_code::success;
        },
        1h,
        5ms,
    };
    int result = -1;
    std::jthread worker{[&](std::stop_token stop) {
        result = monitor.run(stop);
    }};
    const bool observed =
        onedrive::test::wait_until([&] { return rounds >= 2; }, 2s, 5ms);
    worker.request_stop();
    worker.join();
    if (!observed || result != onedrive::exit_code::success) {
        return fail(
            "monitor discarded a user's coalesced create/delete events"
        );
    }
    return EXIT_SUCCESS;
}

int test_transient_probe_is_ignored() {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    std::atomic_int rounds{0};
    onedrive::monitor::Monitor monitor{
        root,
        [&](const std::stop_token&) {
            if (++rounds == 1) {
                const auto probe = root / ".onedrive-cpp-xattr-probe-test";
                std::ofstream{probe} << "probe";
                std::filesystem::remove(probe);
            }
            return onedrive::exit_code::success;
        },
        1h,
        5ms,
    };
    int result = -1;
    std::jthread worker{[&](std::stop_token stop) {
        result = monitor.run(stop);
    }};
    const bool started =
        onedrive::test::wait_until([&] { return rounds >= 1; }, 2s, 5ms);
    std::this_thread::sleep_for(100ms);
    worker.request_stop();
    worker.join();
    if (!started || rounds != 1 || result != onedrive::exit_code::success) {
        return fail("transient internal probes caused a watch feedback loop");
    }
    return EXIT_SUCCESS;
}

int test_failure_log_includes_exception() {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    std::ofstream{root / "file.txt"} << "data";
    FakeItemStore items;
    items.saved_delta_link = "saved";
    FakeGraphClient graph;
    graph.fatal_upload_error = true;
    FakeMetrics metrics;
    auto config = config_for(root, false);
    config.sync_mode = onedrive::sync::SyncMode::bidirectional;
    const onedrive::cli::Console console;
    std::ostringstream messages;
    const auto previous_logger = spdlog::default_logger();
    const auto sink =
        std::make_shared<spdlog::sinks::ostream_sink_mt>(messages);
    spdlog::set_default_logger(
        std::make_shared<spdlog::logger>("watch-race-failure", sink)
    );
    bool failed = false;
    try {
        static_cast<void>(
            onedrive::sync::SyncEngine{config, graph, items, metrics, &console}
                .synchronize()
        );
    } catch (const std::runtime_error&) {
        failed = true;
    }
    spdlog::set_default_logger(previous_logger);
    if (!failed || !messages.str().contains("Synchronization failed after") ||
        !messages.str().contains("simulated fatal upload failure")) {
        return fail("synchronization log omitted the underlying exception");
    }
    return EXIT_SUCCESS;
}

} // namespace

extern "C" {
int __real_fstatat(int, const char*, struct stat*, int);
ssize_t __real_read(int, void*, size_t);
ssize_t __real___read_chk(int, void*, size_t, size_t);

ssize_t __wrap_read(int descriptor, void* buffer, size_t size) {
    if (!prepare_snapshot_read(descriptor)) {
        return -1;
    }
    return __real_read(descriptor, buffer, size);
}

ssize_t __wrap___read_chk(
    int descriptor, void* buffer, size_t size, size_t buffer_size
) {
    if (size <= buffer_size && !prepare_snapshot_read(descriptor)) {
        return -1;
    }
    return __real___read_chk(descriptor, buffer, size, buffer_size);
}

int __wrap_fstatat(
    int directory, const char* path, struct stat* status, int flags
) {
    if (std::strcmp(path, "copied.txt") == 0 && remove_during_identity.load() &&
        identities_before_removal.fetch_sub(1) == 0 &&
        remove_during_identity.exchange(false)) {
        if (::unlinkat(directory, path, 0) == 0) {
            errno = ENOENT;
            return -1;
        }
    }
    return __real_fstatat(directory, path, status, flags);
}
}

int main() {
    onedrive::test::ScenarioProgress progress{84};
    for (const bool tracked : {false, true}) {
        for (const bool upload_only : {false, true}) {
            for (const auto mutation :
                 {SnapshotMutation::size,
                  SnapshotMutation::content,
                  SnapshotMutation::replacement,
                  SnapshotMutation::deletion,
                  SnapshotMutation::read_failure}) {
                if (const auto result = progress.run(
                        std::string{"edit_during_snapshot ("} +
                            "tracked=" + std::to_string(tracked) +
                            ", upload_only=" + std::to_string(upload_only) +
                            ", mutation=" +
                            std::to_string(static_cast<int>(mutation)) + ")",
                        [&] {
                            return test_edit_during_snapshot(
                                tracked, upload_only, mutation
                            );
                        }
                    );
                    result != EXIT_SUCCESS) {
                    return result;
                }
                if (const auto result = progress.run(
                        std::string{"watch_retries_unstable_snapshot ("} +
                            "tracked=" + std::to_string(tracked) +
                            ", upload_only=" + std::to_string(upload_only) +
                            ", mutation=" +
                            std::to_string(static_cast<int>(mutation)) + ")",
                        test_watch_retries_unstable_snapshot
                    );
                    result != EXIT_SUCCESS) {
                    return result;
                }
                if (const auto result = progress.run(
                        std::string{"journaled_modification_is_not_skipped ("} +
                            "tracked=" + std::to_string(tracked) +
                            ", upload_only=" + std::to_string(upload_only) +
                            ", mutation=" +
                            std::to_string(static_cast<int>(mutation)) + ")",
                        test_journaled_modification_is_not_skipped
                    );
                    result != EXIT_SUCCESS) {
                    return result;
                }
            }
        }
    }
    for (const bool download_only : {false, true}) {
        for (const bool metadata_echo : {false, true}) {
            for (const auto mutation :
                 {ReuseMutation::size,
                  ReuseMutation::modified_time,
                  ReuseMutation::atomic_save,
                  ReuseMutation::none}) {
                if (const auto result = progress.run(
                        std::string{"edit_during_delta_reuse ("} +
                            "download_only=" + std::to_string(download_only) +
                            ", metadata_echo=" + std::to_string(metadata_echo) +
                            ", mutation=" +
                            std::to_string(static_cast<int>(mutation)) + ")",
                        [&] {
                            return test_edit_during_delta_reuse(
                                download_only, metadata_echo, mutation
                            );
                        }
                    );
                    result != EXIT_SUCCESS) {
                    return result;
                }
            }
        }
    }
    for (const int identities : {0, 1}) {
        if (const auto result = progress.run(
                std::string{"delete_during_watch_scan ("} +
                    "identities=" + std::to_string(identities) + ")",
                [&] { return test_delete_during_watch_scan(identities); }
            );
            result != EXIT_SUCCESS) {
            return result;
        }
    }
    if (const auto result = progress.run(
            "delete_during_delta_reuse", test_delete_during_delta_reuse
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = progress.run(
            "delete_before_upload_commit", test_delete_before_upload_commit
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = progress.run(
            "delete_metadata_version_race", test_delete_metadata_version_race
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = progress.run(
            "create_delete_during_sync", test_create_delete_during_sync
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = progress.run(
            "transient_probe_is_ignored", test_transient_probe_is_ignored
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    return progress.finish(progress.run(
        "failure_log_includes_exception", test_failure_log_includes_exception
    ));
}
