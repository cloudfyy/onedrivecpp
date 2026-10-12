#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/util/sha256.hpp"

#include <functional>

namespace {

using namespace onedrive;
using namespace onedrive::test::sync;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

struct Fixture {
    TemporaryDirectory temporary;
    config::Config config{config_for(temporary.path() / "sync", false)};
    FakeGraphClient graph;
    FakeItemStore items;
    FakeMetrics metrics;
    cli::Console observer;
    sync::PreviewCache cache;

    int run(bool preview, std::stop_token token = {}) {
        config.dry_run = preview;
        return sync::SyncEngine{
            config, graph, items, metrics, &observer, &cache
        }
            .synchronize(token);
    }

    void seed_cursor() {
        require(run(false) == exit_code::success, "cannot seed saved cursor");
        items.saved_delta_link = "https://graph.example.test/applied";
        items.saved_sync_filter_fingerprint =
            items.applied_delta.sync_filter_fingerprint;
        graph.delta_requests.clear();
    }
};

void test_retained_changes_and_modes() {
    for (const bool saved_cursor : {false, true}) {
        Fixture fixture;
        if (saved_cursor) {
            fixture.seed_cursor();
        }
        fixture.graph.changes = {file("first", "first.txt", 4)};
        fixture.graph.contents["first"] = "data";
        const auto before = fixture.items.apply_count;
        const auto applied_cursor = fixture.items.saved_delta_link;
        require(
            fixture.run(true) == exit_code::success &&
                fixture.cache.available() &&
                fixture.items.apply_count == before &&
                fixture.items.saved_delta_link == applied_cursor &&
                !std::filesystem::exists(
                    fixture.config.sync_data_directory / "first.txt"
                ),
            "preview changed files or applied state instead of retaining "
            "metadata"
        );
        fixture.graph.changes.clear();
        require(
            fixture.run(false) == exit_code::success &&
                fixture.graph.delta_requests ==
                    std::vector<std::optional<std::string>>{
                        applied_cursor, "https://graph.example.test/delta"
                    } &&
                fixture.graph.download_count == 1 &&
                test::read_file(
                    fixture.config.sync_data_directory / "first.txt"
                ) == "data" &&
                fixture.items.applied_delta.apply_mode ==
                    (saved_cursor ? storage::DeltaApplyMode::merge
                                  : storage::DeltaApplyMode::replace) &&
                fixture.items.applied_delta.delta_link ==
                    "https://graph.example.test/delta" &&
                !fixture.cache.available(),
            "sync skipped unapplied preview changes or repeated the original "
            "query"
        );
        fixture.items.saved_delta_link = fixture.items.applied_delta.delta_link;
        fixture.items.saved_sync_filter_fingerprint =
            fixture.items.applied_delta.sync_filter_fingerprint;
        require(
            fixture.run(false) == exit_code::success &&
                fixture.graph.download_count == 1,
            "consumed preview was applied again"
        );
    }
}

void test_latest_changes_and_repeated_preview() {
    Fixture fixture;
    fixture.graph.changes = {
        file("update", "old.txt", 4),
        file("delete", "deleted.txt", 4),
        file("keep", "keep.txt", 4),
    };
    fixture.graph.contents = {
        {"update", "new!"},
        {"delete", "gone"},
        {"keep", "same"},
        {"new", "data"}
    };
    require(fixture.run(true) == 0, "initial preview failed");
    auto updated = file("update", "renamed.txt", 4);
    updated.etag = "new-etag";
    fixture.graph.changes = {
        updated, deleted_item("delete"), file("new", "new.txt", 4)
    };
    fixture.graph.delta_override = [&](const auto&) {
        return graph::DeltaResult{
            .changes = fixture.graph.changes,
            .delta_link = "https://graph.example.test/second-preview",
        };
    };
    require(
        fixture.run(true) == 0 && fixture.cache.available() &&
            fixture.graph.download_count == 0 &&
            fixture.graph.delta_requests.back() ==
                "https://graph.example.test/delta",
        "repeated preview did not advance only its private cursor"
    );
    fixture.graph.changes.clear();
    fixture.graph.delta_override = [](const auto&) {
        return graph::DeltaResult{
            .changes = {},
            .delta_link = "https://graph.example.test/applied-latest",
        };
    };
    require(
        fixture.run(false) == 0 && fixture.graph.download_count == 3 &&
            fixture.graph.delta_requests.back() ==
                "https://graph.example.test/second-preview" &&
            fixture.items.applied_delta.delta_link ==
                "https://graph.example.test/applied-latest" &&
            fixture.items.items.at("update").etag == "new-etag" &&
            !fixture.items.items.contains("delete") &&
            test::read_file(
                fixture.config.sync_data_directory / "renamed.txt"
            ) == "new!" &&
            !std::filesystem::exists(
                fixture.config.sync_data_directory / "old.txt"
            ) &&
            !std::filesystem::exists(
                fixture.config.sync_data_directory / "deleted.txt"
            ) &&
            test::read_file(fixture.config.sync_data_directory / "keep.txt") ==
                "same" &&
            test::read_file(fixture.config.sync_data_directory / "new.txt") ==
                "data",
        "merged metadata lost a change or applied obsolete paths/content"
    );
}

void test_expired_cursor_and_directory_fallback() {
    for (const int change_kind : {0, 1, 2, 3}) {
        Fixture fixture;
        graph::RemoteItem directory{
            .id = "directory",
            .name = "old",
            .parent_id = "root",
            .remote_path = "old",
            .directory = true,
        };
        auto child = file("child", "old/child.txt", 4);
        child.parent_id = "directory";
        fixture.graph.changes = {directory, child};
        fixture.graph.contents["child"] = "data";
        require(fixture.run(true) == 0, "directory preview failed");
        fixture.graph.delta_override = [&](const auto& cursor) {
            if (cursor) {
                if (change_kind == 0) {
                    throw graph::DeltaCursorInvalidError{
                        "expired preview cursor"
                    };
                }
                if (change_kind == 1) {
                    auto moved = directory;
                    moved.name = moved.remote_path = "new";
                    return graph::DeltaResult{
                        .changes = {moved}, .delta_link = "incremental"
                    };
                }
                return graph::DeltaResult{
                    .changes = {deleted_item(
                        change_kind == 2 ? "directory" : "unknown"
                    )},
                    .delta_link = "incremental",
                };
            }
            if (change_kind == 1) {
                directory.name = directory.remote_path = "new";
                child.remote_path = "new/child.txt";
                return graph::DeltaResult{
                    .changes = {directory, child}, .delta_link = "full"
                };
            }
            return graph::DeltaResult{.changes = {}, .delta_link = "full"};
        };
        require(
            fixture.run(false) == 0 &&
                fixture.graph.delta_requests ==
                    std::vector<std::optional<std::string>>{
                        std::nullopt,
                        "https://graph.example.test/delta",
                        std::nullopt
                    } &&
                fixture.items.applied_delta.apply_mode ==
                    storage::DeltaApplyMode::replace &&
                fixture.items.applied_delta.delta_link == "full" &&
                !std::filesystem::exists(
                    fixture.config.sync_data_directory / "old/child.txt"
                ) &&
                fixture.graph.download_count == (change_kind == 1 ? 1 : 0) &&
                !fixture.cache.available(),
            "fallback mixed cached children into the fresh full snapshot"
        );
        if (change_kind == 1) {
            require(
                test::read_file(
                    fixture.config.sync_data_directory / "new/child.txt"
                ) == "data",
                "directory fallback did not download the current child path"
            );
        }
    }
}

void test_scope_state_and_journal_invalidation() {
    using Mutation = std::function<void(Fixture&)>;
    const std::vector<Mutation> mutations{
        [](auto& f) { f.config.drive_id = "other-drive"; },
        [](auto& f) { f.config.sync_data_directory /= "other-account"; },
        [](auto& f) { f.config.state_directory /= "other-state"; },
        [](auto& f) { f.config.graph_endpoint += "/other"; },
        [](auto& f) { f.config.application_id = "other-client"; },
        [](auto& f) { f.config.auth_scope = "Files.Read"; },
        [](auto& f) { f.config.azure_tenant_id = "other-tenant"; },
        [](auto& f) { f.config.auth_endpoint = "https://other.example.test"; },
        [](auto& f) {
            f.config.download_validation =
                config::DownloadValidationMode::relaxed;
        },
        [](auto& f) { f.config.dotfiles = config::DotfilePolicy::exclude; },
        [](auto& f) { f.items.saved_sync_filter_fingerprint = "other-filter"; },
        [](auto& f) {
            f.items.items["tracked"] = {
                .drive_id = "me",
                .remote_id = "tracked",
                .name = "tracked.txt",
                .remote_path = "tracked.txt",
                .local_path = f.config.sync_data_directory / "tracked.txt",
            };
        },
        [](auto& f) {
            f.items.blocked.push_back({
                .drive_id = "me",
                .remote_id = "blocked",
                .name = "blocked.txt",
                .remote_path = "blocked.txt",
                .reason_code = "local_modification",
            });
        },
        [](auto& f) {
            f.items.upload_suppressions_by_path["suppressed"] = {
                .drive_id = "me",
                .remote_id = "suppressed",
                .local_path = f.config.sync_data_directory / "suppressed.txt",
            };
        },
        [](auto& f) { f.items.pending["pending"] = {}; },
        [](auto& f) { f.items.partials["partial"] = {}; },
        [](auto& f) { f.items.pending_uploads_by_path["upload"] = {}; },
        [](auto& f) { f.items.pending_deletes_by_id["delete"] = {}; },
        [](auto& f) { f.items.pending_remote_moves_by_id["remote-move"] = {}; },
        [](auto& f) { f.items.pending_moves_by_id["move"] = {}; },
    };
    for (const auto& mutate : mutations) {
        Fixture fixture;
        fixture.graph.changes = {file("previewed", "previewed.txt", 4)};
        require(
            fixture.run(true) == 0 && fixture.cache.available(),
            "preview failed"
        );
        mutate(fixture);
        fixture.graph.changes.clear();
        const auto result = fixture.run(true);
        require(
            (result == exit_code::success || result == exit_code::blocked) &&
                fixture.graph.delta_requests.back() == std::nullopt,
            "changed scope, state or recovery journal reused preview metadata"
        );
        if (!fixture.items.pending.empty() || !fixture.items.partials.empty() ||
            !fixture.items.pending_uploads_by_path.empty() ||
            !fixture.items.pending_deletes_by_id.empty() ||
            !fixture.items.pending_remote_moves_by_id.empty() ||
            !fixture.items.pending_moves_by_id.empty()) {
            require(
                !fixture.cache.available(),
                "preview retained metadata despite a recovery journal"
            );
        }
    }
    Fixture changed_cursor;
    changed_cursor.seed_cursor();
    require(changed_cursor.run(true) == 0, "cursor preview failed");
    changed_cursor.items.saved_delta_link = "other-applied-cursor";
    require(
        changed_cursor.run(true) == 0 &&
            changed_cursor.graph.delta_requests.back() ==
                "other-applied-cursor",
        "external synchronization cursor change did not invalidate preview"
    );
    Fixture changed_rules;
    changed_rules.config.sync_list =
        changed_rules.temporary.path() / "sync_list";
    test::write_file(*changed_rules.config.sync_list, "first.txt\n");
    changed_rules.graph.changes = {file("first", "first.txt", 4)};
    require(
        changed_rules.run(true) == 0 && changed_rules.cache.available(),
        "selective preview failed"
    );
    test::write_file(*changed_rules.config.sync_list, "other.txt\n");
    changed_rules.graph.changes.clear();
    require(
        changed_rules.run(true) == 0 &&
            changed_rules.graph.delta_requests.back() == std::nullopt,
        "changed sync-list contents reused a preview with obsolete rules"
    );
}

void test_state_changes_during_preview_are_not_cached() {
    Fixture fixture;
    fixture.graph.delta_override = [&](const auto&) {
        fixture.items.saved_delta_link = "external-cursor";
        return graph::DeltaResult{
            .changes = {file("first", "first.txt", 4)},
            .delta_link = "preview-cursor",
        };
    };
    require(
        fixture.run(true) == 0 && !fixture.cache.available() &&
            fixture.items.saved_delta_link == "external-cursor" &&
            fixture.items.apply_count == 0,
        "preview published metadata against an externally changed baseline"
    );
}

void test_local_revalidation_and_unsuccessful_tasks() {
    Fixture conflict;
    conflict.graph.changes = {file("first", "first.txt", 4)};
    conflict.graph.contents["first"] = "data";
    require(conflict.run(true) == 0, "conflict preview failed");
    std::filesystem::create_directories(conflict.config.sync_data_directory);
    test::write_file(
        conflict.config.sync_data_directory / "first.txt", "local edit"
    );
    conflict.graph.changes.clear();
    require(
        conflict.run(false) == exit_code::blocked &&
            test::read_file(
                conflict.config.sync_data_directory / "first.txt"
            ) == "local edit" &&
            conflict.graph.download_count == 0 &&
            conflict.items.applied_delta.blocked_upserts.at(0).reason_code ==
                "local_modification" &&
            !conflict.cache.available(),
        "reused metadata bypassed fresh local conflict checks"
    );
    for (const bool preview : {false, true}) {
        Fixture cancelled;
        cancelled.graph.changes = {file("first", "first.txt", 4)};
        require(cancelled.run(true) == 0, "cancellation preview failed");
        std::stop_source stop;
        cancelled.graph.stop_on_delta = &stop;
        require(
            cancelled.run(preview, stop.get_token()) == exit_code::cancelled &&
                !cancelled.cache.available() &&
                cancelled.items.apply_count == 0,
            "cancelled task retained a cache or committed the cursor"
        );
        cancelled.graph.stop_on_delta = nullptr;
        require(
            cancelled.run(true) == 0 && cancelled.cache.available(),
            "cannot restore preview before failure"
        );
        cancelled.graph.delta_override = [](const auto&) -> graph::DeltaResult {
            throw std::runtime_error{"simulated network failure"};
        };
        require(
            test::throws_with<std::runtime_error>(
                [&] { static_cast<void>(cancelled.run(preview)); },
                "simulated network failure"
            ) && !cancelled.cache.available(),
            "failed task published partial preview metadata"
        );
    }
}

void test_local_deletions_are_replanned_and_guarded() {
    Fixture fixture;
    fixture.graph.changes = {
        file("a", "a.txt", 4),
        file("b", "b.txt", 4),
        file("c", "c.txt", 4),
    };
    fixture.graph.contents = {{"a", "data"}, {"b", "data"}, {"c", "data"}};
    require(fixture.run(false) == 0, "cannot seed files for deletion guard");
    fixture.items.saved_delta_link = fixture.items.applied_delta.delta_link;
    fixture.items.saved_sync_filter_fingerprint =
        fixture.items.applied_delta.sync_filter_fingerprint;
    fixture.config.sync_mode = sync::SyncMode::bidirectional;
    fixture.config.maximum_remote_deletions = 2;
    fixture.graph.changes.clear();
    require(
        fixture.run(true) == 0 && fixture.cache.available(),
        "deletion preview failed"
    );
    for (const auto* name : {"a.txt", "b.txt", "c.txt"}) {
        std::filesystem::remove(fixture.config.sync_data_directory / name);
    }
    require(
        test::throws_with<std::runtime_error>(
            [&] { static_cast<void>(fixture.run(false)); },
            "large-delete safeguard"
        ) &&
            fixture.graph.delta_requests.back() ==
                fixture.items.saved_delta_link &&
            fixture.graph.deleted_items.empty() &&
            fixture.items.pending_deletes_by_id.empty() &&
            fixture.items.items.size() == 3 && !fixture.cache.available(),
        "preview reuse bypassed replanning or the remote-deletion safety limit"
    );
}

void test_full_snapshot_reuse_removes_missing_tracked_items() {
    Fixture fixture;
    fixture.graph.changes = {
        file("keep", "keep.txt", 4), file("gone", "gone.txt", 4)
    };
    fixture.graph.contents = {
        {"keep", "data"}, {"gone", "data"}, {"new", "data"}
    };
    require(fixture.run(false) == 0, "cannot seed replacement state");
    fixture.graph.changes = {file("keep", "keep.txt", 4)};
    require(
        fixture.run(true) == 0 && fixture.cache.available() &&
            std::filesystem::exists(
                fixture.config.sync_data_directory / "gone.txt"
            ),
        "replacement preview modified tracked files"
    );
    fixture.graph.changes = {file("new", "new.txt", 4)};
    require(
        fixture.run(false) == 0 &&
            fixture.graph.delta_requests.back() ==
                "https://graph.example.test/delta" &&
            fixture.items.applied_delta.apply_mode ==
                storage::DeltaApplyMode::replace &&
            !fixture.items.items.contains("gone") &&
            !std::filesystem::exists(
                fixture.config.sync_data_directory / "gone.txt"
            ) &&
            test::read_file(fixture.config.sync_data_directory / "keep.txt") ==
                "data" &&
            test::read_file(fixture.config.sync_data_directory / "new.txt") ==
                "data",
        "cached full snapshot lost synthetic deletions during incremental merge"
    );
}

void test_tracked_directory_fallback() {
    for (const bool deletion : {false, true}) {
        Fixture fixture;
        graph::RemoteItem directory{
            .id = "directory",
            .name = "old",
            .etag = "directory-etag",
            .parent_id = "root",
            .remote_path = "old",
            .directory = true,
        };
        auto child = file("child", "old/child.txt", 4);
        child.parent_id = "directory";
        const auto previewed = file("previewed", "previewed.txt", 4);
        fixture.graph.changes = {directory, child};
        fixture.graph.contents = {{"child", "data"}, {"previewed", "data"}};
        require(fixture.run(false) == 0, "cannot seed tracked directory");
        fixture.items.saved_delta_link = fixture.items.applied_delta.delta_link;
        fixture.items.saved_sync_filter_fingerprint =
            fixture.items.applied_delta.sync_filter_fingerprint;
        fixture.graph.changes = {previewed};
        require(fixture.run(true) == 0, "incremental directory preview failed");
        fixture.graph.delta_override = [&](const auto& cursor) {
            auto moved = directory;
            moved.name = moved.remote_path = "new";
            if (cursor) {
                return graph::DeltaResult{
                    .changes = {deletion ? deleted_item("directory") : moved},
                    .delta_link = "incremental",
                };
            }
            if (deletion) {
                return graph::DeltaResult{
                    .changes = {previewed}, .delta_link = "full"
                };
            }
            child.remote_path = "new/child.txt";
            return graph::DeltaResult{
                .changes = {moved, child, previewed}, .delta_link = "full"
            };
        };
        require(
            fixture.run(false) == 0 &&
                fixture.graph.delta_requests.back() == std::nullopt &&
                fixture.items.applied_delta.delta_link == "full" &&
                !std::filesystem::exists(
                    fixture.config.sync_data_directory / "old"
                ) &&
                test::read_file(
                    fixture.config.sync_data_directory / "previewed.txt"
                ) == "data" &&
                !fixture.cache.available(),
            "directory absent from cached delta bypassed tracked-state fallback"
        );
        if (!deletion) {
            require(
                test::read_file(
                    fixture.config.sync_data_directory / "new/child.txt"
                ) == "data",
                "tracked directory fallback lost its child"
            );
        }
    }
}

void test_snapshot_order_and_metadata_equality() {
    Fixture fixture;
    auto first = file("first", "first.txt", 4);
    first.content_hash = util::FileHash{
        .algorithm = util::FileHashAlgorithm::sha256,
        .value = util::sha256_hex("data"),
    };
    fixture.graph.changes = {first, file("second", "second.txt", 4)};
    fixture.graph.contents = {{"first", "data"}, {"second", "data"}};
    require(fixture.run(false) == 0, "cannot seed hashed state");
    fixture.items.saved_delta_link = fixture.items.applied_delta.delta_link;
    fixture.items.saved_sync_filter_fingerprint =
        fixture.items.applied_delta.sync_filter_fingerprint;
    for (const auto* id : {"blocked-a", "blocked-b"}) {
        fixture.items.blocked.push_back({
            .drive_id = "me",
            .remote_id = id,
            .name = id,
            .remote_path = id,
            .reason_code = "malware_detected",
        });
        fixture.items.upload_suppressions_by_path[id] = {
            .drive_id = "me",
            .remote_id = "first",
            .local_path = fixture.config.sync_data_directory / id,
        };
    }
    fixture.graph.delta_override = [](const auto&) {
        return graph::DeltaResult{
            .changes = {},
            .delta_link = "https://graph.example.test/private-preview",
        };
    };
    require(
        fixture.run(true) == exit_code::blocked && fixture.cache.available(),
        "complete blocked preview did not retain reusable raw metadata"
    );
    const auto original_order = fixture.items.drive_items("me");
    fixture.items.reverse_state_order = true;
    const auto reversed = fixture.items.drive_items("me");
    require(
        original_order.front().remote_id == reversed.back().remote_id &&
            fixture.run(true) == exit_code::blocked &&
            fixture.graph.delta_requests.back() ==
                "https://graph.example.test/private-preview",
        "collection order alone invalidated an equivalent snapshot"
    );
    fixture.items.items.at("first").content_hash->value =
        util::sha256_hex("changed");
    require(
        fixture.run(true) == exit_code::blocked &&
            fixture.graph.delta_requests.back() ==
                fixture.items.saved_delta_link,
        "changed persisted content hash reused a snapshot with the same cursor"
    );
}

class StopOnCompletion final : public events::Observer {
public:
    explicit StopOnCompletion(std::stop_source& stop)
        : stop_{stop} {
    }

private:
    void on_event(const events::Event& event) const override {
        const auto* message = std::get_if<events::MessageEvent>(&event);
        if (message && message->event == "sync_completed") {
            stop_.request_stop();
        }
    }
    std::stop_source& stop_;
};

void test_cache_publication_and_fallback_failures() {
    Fixture empty_cursor;
    empty_cursor.graph.delta_override = [](const auto&) {
        return graph::DeltaResult{};
    };
    require(
        empty_cursor.run(true) == 0 && !empty_cursor.cache.available(),
        "preview cached an unusable empty cursor"
    );

    Fixture late_stop;
    late_stop.config.dry_run = true;
    std::stop_source stop;
    StopOnCompletion observer{stop};
    require(
        sync::SyncEngine{
            late_stop.config,
            late_stop.graph,
            late_stop.items,
            late_stop.metrics,
            &observer,
            &late_stop.cache
        }
                    .synchronize(stop.get_token()) == 0 &&
            stop.stop_requested() && !late_stop.cache.available(),
        "late cancellation published a preview cache after completion"
    );
    for (const bool cancellation : {false, true}) {
        Fixture fixture;
        fixture.graph.changes = {file("first", "first.txt", 4)};
        require(fixture.run(true) == 0, "fallback failure preview failed");
        fixture.graph.delta_override =
            [cancellation](const auto& cursor) -> graph::DeltaResult {
            if (cursor) {
                throw graph::DeltaCursorInvalidError{"expired"};
            }
            if (cancellation) {
                throw graph::RequestCancelledError{"fallback cancelled"};
            }
            throw std::runtime_error{"fallback failed"};
        };
        if (cancellation) {
            require(
                fixture.run(false) == exit_code::cancelled,
                "cancelled full fallback was reported as success"
            );
        } else {
            require(
                test::throws_with<std::runtime_error>(
                    [&] { static_cast<void>(fixture.run(false)); },
                    "fallback failed"
                ),
                "failed full fallback used the old cached result"
            );
        }
        require(
            !fixture.cache.available() && fixture.items.apply_count == 0 &&
                fixture.graph.delta_requests.back() == std::nullopt,
            "unsuccessful fallback retained cache or committed state"
        );
    }
}

void test_cached_selective_exclusions_preserve_local_content() {
    for (const int local_kind : {0, 1, 2}) {
        Fixture fixture;
        fixture.graph.changes = {file("first", "first.txt", 4)};
        fixture.graph.contents["first"] = "data";
        require(
            fixture.run(false) == 0, "cannot seed selectively retained file"
        );
        const auto local = fixture.config.sync_data_directory / "first.txt";
        if (local_kind != 0) {
            std::filesystem::remove(local);
            if (local_kind == 2) {
                std::filesystem::create_directory(local);
            }
        }
        fixture.config.sync_list = fixture.temporary.path() / "sync_list";
        test::write_file(*fixture.config.sync_list, "first.txt\n");
        fixture.graph.changes = {file("first", "excluded.txt", 4)};
        require(
            fixture.run(true) == 0 && fixture.cache.available(),
            "selective exclusion preview failed"
        );
        fixture.graph.changes.clear();
        require(
            fixture.run(false) == 0 &&
                fixture.items.applied_delta.apply_mode ==
                    storage::DeltaApplyMode::replace &&
                !fixture.items.items.contains("first") &&
                !std::filesystem::exists(
                    fixture.config.sync_data_directory / "excluded.txt"
                ) &&
                fixture.graph.download_count == 1 &&
                fixture.items.applied_delta.upload_suppressions.size() ==
                    (local_kind == 0 ? 1U : 0U),
            "cached replacement changed selectively retained content or "
            "suppression"
        );
        if (local_kind == 0) {
            require(
                test::read_file(local) == "data",
                "selective exclusion removed retained local bytes"
            );
        } else if (local_kind == 2) {
            require(
                std::filesystem::is_directory(local),
                "selective exclusion removed a user-created directory"
            );
        }
    }
}

void test_selectively_retained_path_error_clears_cache() {
    Fixture fixture;
    graph::RemoteItem directory{
        .id = "directory",
        .name = "Parent",
        .parent_id = "root",
        .remote_path = "Parent",
        .directory = true,
    };
    auto child = file("child", "Parent/child.txt", 4);
    child.parent_id = "directory";
    fixture.graph.changes = {directory, child};
    fixture.graph.contents["child"] = "data";
    require(fixture.run(false) == 0, "cannot seed selectively retained tree");
    fixture.config.sync_list = fixture.temporary.path() / "sync_list";
    test::write_file(*fixture.config.sync_list, "Parent\n");
    require(
        fixture.run(true) == 0 && fixture.cache.available(),
        "cannot preview selected directory"
    );

    const auto parent = fixture.config.sync_data_directory / "Parent";
    std::filesystem::remove(parent / "child.txt");
    std::filesystem::remove(parent);
    test::write_file(parent, "user replacement");
    directory.name = directory.remote_path = "Excluded";
    child.remote_path = "Excluded/child.txt";
    fixture.graph.changes = {directory, child};
    const auto applied = fixture.items.apply_count;
    require(
        test::throws_with<std::runtime_error>(
            [&] { static_cast<void>(fixture.run(false)); },
            "cannot inspect selectively retained local file"
        ) && !fixture.cache.available() &&
            fixture.items.apply_count == applied &&
            fixture.graph.download_count == 1 &&
            test::read_file(parent) == "user replacement",
        "retained-path failure used cached metadata or changed user "
        "content/state"
    );
}

} // namespace

int main() {
    try {
        test_retained_changes_and_modes();
        test_latest_changes_and_repeated_preview();
        test_expired_cursor_and_directory_fallback();
        test_scope_state_and_journal_invalidation();
        test_state_changes_during_preview_are_not_cached();
        test_local_revalidation_and_unsuccessful_tasks();
        test_local_deletions_are_replanned_and_guarded();
        test_full_snapshot_reuse_removes_missing_tracked_items();
        test_tracked_directory_fallback();
        test_snapshot_order_and_metadata_equality();
        test_cache_publication_and_fallback_failures();
        test_cached_selective_exclusions_preserve_local_content();
        test_selectively_retained_path_error_clears_cache();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        return onedrive::test::fail(error.what());
    }
}
