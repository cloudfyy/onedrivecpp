#include "support/progress.hpp"
#include "support.hpp"
#include "onedrive/storage/item_database.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/util/sha256.hpp"
#include "sync/download/integrity.hpp"

#include <nlohmann/json.hpp>

namespace {

using namespace onedrive;
using namespace onedrive::test::sync;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

class StopOnEvent final : public events::Observer {
public:
    StopOnEvent(std::stop_source& stop, bool after_file)
        : stop_{stop},
          after_file_{after_file} {
    }

private:
    void on_event(const events::Event& event) const override {
        if (const auto* progress =
                std::get_if<events::DownloadProgressEvent>(&event);
            after_file_ && progress && progress->completed_files == 1) {
            stop_.request_stop();
        }
        if (const auto* message = std::get_if<events::MessageEvent>(&event);
            !after_file_ && message && message->event == "execution_started") {
            stop_.request_stop();
        }
    }
    std::stop_source& stop_;
    bool after_file_;
};

struct Fixture {
    test::TemporaryDirectory temporary;
    config::Config config{config_for(temporary.path() / "sync", false)};
    FakeMetrics metrics;
    cli::Console observer;

    Fixture() {
        config.download_concurrency = 1;
    }

    account::DriveIdentity identity() const {
        return test::test_drive_identity(config.drive_id, config.drive_id);
    }

    int
    run(FakeGraphClient& graph,
        const events::Observer* active = nullptr,
        std::stop_token token = {}) {
        storage::ItemDatabase database{config.state_directory, identity()};
        database.open();
        return sync::SyncEngine{
            config, graph, database, metrics, active ? active : &observer
        }
            .synchronize(token);
    }

    std::optional<storage::PendingDelta> pending() const {
        storage::ItemDatabase database{config.state_directory, identity()};
        database.open();
        return database.pending_delta(config.drive_id);
    }

    void interrupt(FakeGraphClient& graph, bool after_file = false) {
        std::stop_source stop;
        StopOnEvent active{stop, after_file};
        require(
            run(graph, &active, stop.get_token()) == exit_code::cancelled &&
                pending().has_value(),
            "interruption did not persist the fetched batch"
        );
    }
};

void test_restart_preserves_modes_and_latest_changes() {
    for (const bool incremental : {false, true}) {
        Fixture fixture;
        std::optional<std::string> applied;
        if (incremental) {
            FakeGraphClient seed;
            require(fixture.run(seed) == 0, "cannot seed applied cursor");
            applied = "https://graph.example.test/delta";
        }
        FakeGraphClient first;
        first.delta_override = [](const auto&) {
            return graph::DeltaResult{
                .changes =
                    {file("first", "first.txt", 4),
                     file("second", "second.txt", 4),
                     file("gone", "gone.txt", 4)},
                .delta_link = "https://graph.example.test/fetched",
            };
        };
        first.contents["first"] = "same";
        fixture.interrupt(first, true);
        const auto pending = fixture.pending();
        require(
            pending->applied_delta_link == applied &&
                pending->apply_mode == (incremental
                                            ? storage::DeltaApplyMode::merge
                                            : storage::DeltaApplyMode::replace),
            "pending batch lost its original apply mode or baseline"
        );
        {
            storage::ItemDatabase database{
                fixture.config.state_directory, fixture.identity()
            };
            database.open();
            require(
                database.delta_link(fixture.config.drive_id) == applied &&
                    database.find(fixture.config.drive_id, "first").has_value(),
                "interruption advanced the cursor or lost completed item state"
            );
        }
        FakeGraphClient resumed;
        resumed.delta_override = [](const auto& cursor) {
            require(
                cursor == "https://graph.example.test/fetched",
                "restart repeated the old delta query"
            );
            auto changed = file("second", "latest.txt", 4);
            changed.etag = "new-etag";
            auto removed = file("gone", "gone.txt", 4);
            removed.deleted = true;
            return graph::DeltaResult{
                .changes = {changed, removed, file("new", "new.txt", 2)},
                .delta_link = "https://graph.example.test/latest",
            };
        };
        resumed.contents = {{"second", "new!"}, {"new", "ok"}};
        require(
            fixture.run(resumed) == 0 && resumed.download_count == 2 &&
                !fixture.pending() &&
                test::read_file(
                    fixture.config.sync_data_directory / "first.txt"
                ) == "same" &&
                test::read_file(
                    fixture.config.sync_data_directory / "latest.txt"
                ) == "new!" &&
                test::read_file(
                    fixture.config.sync_data_directory / "new.txt"
                ) == "ok" &&
                !std::filesystem::exists(
                    fixture.config.sync_data_directory / "second.txt"
                ) &&
                !std::filesystem::exists(
                    fixture.config.sync_data_directory / "gone.txt"
                ),
            "restart lost changes, ignored newer metadata or redownloaded "
            "completed content"
        );
        storage::ItemDatabase database{
            fixture.config.state_directory, fixture.identity()
        };
        database.open();
        require(
            database.delta_link(fixture.config.drive_id) ==
                "https://graph.example.test/latest",
            "restart did not commit the refreshed cursor"
        );
    }
}

void test_empty_refresh_preview_and_refresh_failure() {
    Fixture fixture;
    FakeGraphClient first;
    first.changes = {file("first", "first.txt", 4)};
    fixture.interrupt(first);
    const auto fetched = fixture.pending()->delta.delta_link;
    FakeGraphClient failing;
    failing.delta_override = [](const auto&) -> graph::DeltaResult {
        throw std::runtime_error{"refresh failed"};
    };
    require(
        test::throws_with<std::runtime_error>(
            [&] { static_cast<void>(fixture.run(failing)); }, "refresh failed"
        ) && fixture.pending()->delta.delta_link == fetched &&
            failing.download_count == 0,
        "failed refresh executed stale metadata or destroyed recovery state"
    );
    FakeGraphClient refreshed;
    refreshed.delta_override = [fetched](const auto& cursor) {
        require(cursor == fetched, "refresh ignored the persisted cursor");
        return graph::DeltaResult{
            .changes = {}, .delta_link = "https://graph.example.test/refreshed"
        };
    };
    refreshed.contents["first"] = "data";
    fixture.config.dry_run = true;
    require(
        fixture.run(refreshed) == 0 && refreshed.download_count == 0 &&
            fixture.pending()->delta.delta_link == fetched,
        "preview modified durable pending metadata or files"
    );
    fixture.config.dry_run = false;
    require(
        fixture.run(refreshed) == 0 && refreshed.download_count == 1 &&
            !fixture.pending() &&
            test::read_file(fixture.config.sync_data_directory / "first.txt") ==
                "data",
        "empty incremental refresh dropped unapplied cached changes"
    );
}

void test_scope_and_baseline_invalidation() {
    for (int kind = 0; kind != 6; ++kind) {
        Fixture fixture;
        FakeGraphClient first;
        first.changes = {file("old", "old.txt", 4)};
        fixture.interrupt(first);
        auto pending = *fixture.pending();
        if (kind == 0) {
            fixture.config.graph_endpoint = "https://other.example.test/v1.0";
        } else if (kind == 1) {
            fixture.config.sync_data_directory =
                fixture.temporary.path() / "other-root";
        } else if (kind == 2) {
            fixture.config.application_id = "other-client";
        } else if (kind == 3) {
            pending.sync_filter_fingerprint = "other-filter";
        } else if (kind == 4) {
            pending.applied_delta_link = "different-applied-cursor";
        } else {
            pending.applied_filter_fingerprint = "different-applied-filter";
        }
        {
            storage::ItemDatabase database{
                fixture.config.state_directory, fixture.identity()
            };
            database.open();
            database.save_pending_delta(pending);
        }
        FakeGraphClient fresh;
        fresh.changes = {file("new", "new.txt", 4)};
        fresh.contents["new"] = "data";
        require(
            fixture.run(fresh) == 0 &&
                fresh.delta_requests ==
                    std::vector<std::optional<std::string>>{std::nullopt} &&
                fresh.download_count == 1 &&
                !std::filesystem::exists(
                    fixture.config.sync_data_directory / "old.txt"
                ) &&
                !fixture.pending(),
            "incompatible pending metadata was reused"
        );
    }
}

void test_cursor_and_hierarchy_fallback() {
    for (const bool expired : {false, true}) {
        Fixture fixture;
        FakeGraphClient first;
        auto directory = file("dir", "old", 0);
        directory.directory = true;
        auto child = file("stale", "old/stale.txt", 4);
        child.parent_id = "dir";
        first.changes = {directory, child};
        fixture.interrupt(first);
        FakeGraphClient fresh;
        fresh.delta_override = [expired,
                                directory](const auto& cursor) mutable {
            if (cursor) {
                if (expired) {
                    throw graph::DeltaCursorInvalidError{
                        "expired pending cursor"
                    };
                }
                directory.name = directory.remote_path = "new";
                return graph::DeltaResult{
                    .changes = {directory}, .delta_link = "incremental"
                };
            }
            directory.name = directory.remote_path = "new";
            auto current = file("current", "new/current.txt", 4);
            current.parent_id = "dir";
            return graph::DeltaResult{
                .changes = {directory, current},
                .delta_link = "https://graph.example.test/full"
            };
        };
        fresh.contents["current"] = "data";
        require(
            fixture.run(fresh) == 0 && fresh.delta_requests.size() == 2 &&
                fresh.delta_requests.back() == std::nullopt &&
                fresh.download_count == 1 && !fixture.pending() &&
                !std::filesystem::exists(
                    fixture.config.sync_data_directory / "old/stale.txt"
                ) &&
                test::read_file(
                    fixture.config.sync_data_directory / "new/current.txt"
                ) == "data",
            "full fallback mixed stale cached descendants into fresh metadata"
        );
    }
}

void test_partial_download_survives_restart() {
    Fixture fixture;
    FakeGraphClient first;
    first.changes = {file("partial", "partial.txt", 4)};
    first.contents["partial"] = "data";
    first.cancellable_id = "partial";
    first.cancellation_checkpoint = 2;
    std::stop_source stop;
    std::jthread cancel{[&](std::stop_token token) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (first.checkpoint_count == 0 && !token.stop_requested() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        stop.request_stop();
    }};
    require(
        fixture.run(first, nullptr, stop.get_token()) == exit_code::cancelled &&
            fixture.pending().has_value(),
        "partial cancellation lost pending delta metadata"
    );
    cancel.join();
    {
        storage::ItemDatabase database{
            fixture.config.state_directory, fixture.identity()
        };
        database.open();
        const auto partial =
            database.partial_download(fixture.config.drive_id, "partial");
        require(
            partial && partial->completed_bytes == 2 &&
                std::filesystem::file_size(partial->temporary_path) == 2,
            "partial download checkpoint was not durable"
        );
    }
    FakeGraphClient resumed;
    resumed.changes.clear();
    resumed.contents["partial"] = "data";
    require(
        fixture.run(resumed) == 0 && resumed.last_download_offset == 2 &&
            resumed.delta_requests ==
                std::vector<std::optional<std::string>>{
                    "https://graph.example.test/delta"
                } &&
            !fixture.pending() &&
            test::read_file(
                fixture.config.sync_data_directory / "partial.txt"
            ) == "data",
        "restart discarded partial bytes or repeated a full delta query"
    );
}

void test_legacy_hash_policy_refresh_preserves_downloads() {
    for (const bool incremental : {false, true}) {
        Fixture fixture;
        if (incremental) {
            FakeGraphClient seed;
            require(
                fixture.run(seed) == 0, "cannot seed legacy applied cursor"
            );
        }
        auto completed = file("completed", "completed.txt", 4);
        completed.content_hash = util::FileHash{
            .algorithm = util::FileHashAlgorithm::sha256,
            .value = util::sha256_hex("same"),
        };
        auto partial = file("partial", "partial.one", 4);
        partial.content_hash = util::FileHash{
            .algorithm = util::FileHashAlgorithm::sha256,
            .value = std::string(64, '0'),
        };
        FakeGraphClient first;
        first.changes = {completed, partial};
        first.contents["completed"] = "same";
        fixture.interrupt(first, true);
        auto pending = *fixture.pending();
        const auto applied = pending.applied_delta_link;
        const auto& config = fixture.config;
        pending.scope_fingerprint = util::sha256_hex(
            nlohmann::json::array(
                {
                    config.sync_data_directory.string(),
                    config.state_directory.string(),
                    config.drive_id,
                    config.graph_endpoint,
                    config.application_id,
                    config.azure_tenant_id,
                    config.auth_endpoint,
                    config.auth_scope,
                    static_cast<int>(config.download_validation),
                }
            )
                .dump()
        );
        const auto destination = config.sync_data_directory / "partial.one";
        const auto temporary =
            config.sync_data_directory / ".partial.one.onedrive-partial-legacy";
        test::write_file(temporary, "da");
        {
            storage::ItemDatabase database{
                config.state_directory, fixture.identity()
            };
            database.open();
            database.save_pending_delta(pending);
            database.save_partial_download({
                .item =
                    {
                        .drive_id = config.drive_id,
                        .remote_id = partial.id,
                        .name = partial.name,
                        .etag = partial.etag,
                        .remote_path = partial.remote_path,
                        .local_path = destination,
                        .size = partial.size,
                    },
                .temporary_path = temporary,
                .completed_bytes = 2,
            });
        }
        completed.content_hash = util::FileHash{
            .algorithm = util::FileHashAlgorithm::quick_xor,
            .value = sync::detail::quick_xor_hash(
                config.sync_data_directory / "completed.txt"
            ),
        };
        partial.content_hash = util::FileHash{
            .algorithm = util::FileHashAlgorithm::quick_xor,
            .value = "ZAgDHcIAAAAAAAAABAAAAAAAAAA=",
        };
        FakeGraphClient fresh;
        fresh.delta_override = [&](const auto& cursor) {
            require(
                cursor == applied, "legacy hash policy reused the failed cursor"
            );
            return graph::DeltaResult{
                .changes = {completed, partial},
                .delta_link = "https://graph.example.test/refreshed",
            };
        };
        fresh.contents["partial"] = "data";
        require(
            fixture.run(fresh) == 0 && fresh.download_count == 1 &&
                fresh.last_download_offset == 2 && !fixture.pending() &&
                test::read_file(config.sync_data_directory / "completed.txt") ==
                    "same" &&
                test::read_file(destination) == "data" &&
                !std::filesystem::exists(temporary),
            "legacy hash refresh discarded installed files or partial bytes"
        );
        storage::ItemDatabase database{
            config.state_directory, fixture.identity()
        };
        database.open();
        for (const auto& id : {"completed", "partial"}) {
            const auto state = database.find(config.drive_id, id);
            require(
                state && state->content_hash &&
                    state->content_hash->algorithm ==
                        util::FileHashAlgorithm::quick_xor,
                "refreshed snapshot retained unsupported Graph SHA-256"
            );
        }
    }
}

void test_persistence_failure_stops_before_downloads() {
    Fixture fixture;
    FakeItemStore store;
    store.fail_save_pending_delta = true;
    FakeGraphClient graph;
    graph.changes = {file("file", "file.txt", 4)};
    require(
        test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(sync::SyncEngine{
                    fixture.config,
                    graph,
                    store,
                    fixture.metrics,
                    &fixture.observer
                }
                                      .synchronize());
            },
            "simulated pending delta persistence failure"
        ) && graph.download_count == 0 &&
            store.apply_count == 0,
        "failed durable snapshot persistence still executed downloads"
    );
}

} // namespace

int main() {
    onedrive::test::ScenarioProgress progress{7};
    try {
        progress.run(
            "restart_preserves_modes_and_latest_changes",
            test_restart_preserves_modes_and_latest_changes
        );
        progress.run(
            "empty_refresh_preview_and_refresh_failure",
            test_empty_refresh_preview_and_refresh_failure
        );
        progress.run(
            "scope_and_baseline_invalidation",
            test_scope_and_baseline_invalidation
        );
        progress.run(
            "cursor_and_hierarchy_fallback", test_cursor_and_hierarchy_fallback
        );
        progress.run(
            "partial_download_survives_restart",
            test_partial_download_survives_restart
        );
        progress.run(
            "legacy_hash_policy_refresh_preserves_downloads",
            test_legacy_hash_policy_refresh_preserves_downloads
        );
        progress.run(
            "persistence_failure_stops_before_downloads",
            test_persistence_failure_stops_before_downloads
        );
        return progress.finish();
    } catch (const std::exception& error) {
        return onedrive::test::fail(error.what());
    }
}
