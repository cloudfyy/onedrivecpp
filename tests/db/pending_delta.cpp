#include "support.hpp"
#include "onedrive/storage/item_database.hpp"
#include "onedrive/storage/status.hpp"
#include "support/common.hpp"

#include <tuple>

namespace {

using namespace onedrive;
using namespace onedrive::test::db;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

auto fields(const graph::RemoteItem& item) {
    return std::tie(
        item.id,
        item.name,
        item.etag,
        item.ctag,
        item.parent_id,
        item.remote_path,
        item.last_modified,
        item.size,
        item.directory,
        item.deleted,
        item.root,
        item.malware,
        item.content_hash,
        item.validate_content,
        item.unsupported_format
    );
}

storage::PendingDelta sample() {
    return {
        .drive_id = "me",
        .scope_fingerprint = "scope",
        .sync_filter_fingerprint = "filter",
        .applied_delta_link = "applied",
        .applied_filter_fingerprint = "",
        .delta =
            {
                .changes =
                    {
                        {.id = "first",
                         .name = "first.txt",
                         .etag = "etag",
                         .ctag = "ctag",
                         .parent_id = "dir",
                         .remote_path = "dir/first.txt",
                         .last_modified = "2026-10-02T00:00:00Z",
                         .size = 4,
                         .content_hash =
                             util::FileHash{
                                 util::FileHashAlgorithm::sha256, "hash"
                             },
                         .validate_content = false},
                        {.id = "dir",
                         .name = "dir",
                         .remote_path = "dir",
                         .directory = true,
                         .root = true,
                         .unsupported_format = "onenote"},
                        {.id = "gone",
                         .deleted = true,
                         .malware = true,
                         .content_hash =
                             util::FileHash{
                                 util::FileHashAlgorithm::quick_xor, "quick"
                             }},
                        {.id = "sha1",
                         .name = "sha1.one",
                         .etag = "sha1-etag",
                         .content_hash =
                             util::FileHash{
                                 util::FileHashAlgorithm::sha1,
                                 "a17c9aaa61e80a1bf71d0d850af4e5baa9800bbd"
                             }},
                    },
                .delta_link = "fetched",
            },
        .apply_mode = storage::DeltaApplyMode::replace,
    };
}

void test_round_trip_and_transactions() {
    test::TemporaryDirectory temporary;
    const auto expected = sample();
    {
        storage::ItemDatabase database{temporary.path(), identity()};
        database.open();
        database.apply_delta({.drive_id = "me", .delta_link = "applied"});
        database.save_pending_delta(expected);
        require(
            database.delta_link("me") == "applied",
            "saving fetched metadata advanced the applied cursor"
        );
    }
    {
        storage::ItemDatabase database{temporary.path(), identity()};
        database.open();
        auto loaded = database.pending_delta("me");
        const auto summary =
            storage::read_state_summary(temporary.path(), "me");
        require(
            summary.pending_deltas == 1 && summary.pending_delta_changes == 4,
            "status did not expose pending remote metadata"
        );
        require(
            loaded && loaded->drive_id == expected.drive_id &&
                loaded->scope_fingerprint == expected.scope_fingerprint &&
                loaded->sync_filter_fingerprint ==
                    expected.sync_filter_fingerprint &&
                loaded->applied_delta_link == expected.applied_delta_link &&
                loaded->applied_filter_fingerprint ==
                    expected.applied_filter_fingerprint &&
                loaded->apply_mode == expected.apply_mode &&
                loaded->delta.delta_link == expected.delta.delta_link &&
                loaded->delta.changes.size() == expected.delta.changes.size() &&
                !database.pending_delta("another-drive"),
            "pending delta header or drive isolation did not survive restart"
        );
        for (std::size_t index = 0; index < expected.delta.changes.size();
             ++index) {
            require(
                fields(loaded->delta.changes[index]) ==
                    fields(expected.delta.changes[index]),
                "pending metadata lost fields or record order"
            );
        }
        auto invalid = expected;
        invalid.delta.delta_link = "replacement";
        invalid.delta.changes.push_back(invalid.delta.changes.front());
        require(
            test::throws_with<std::runtime_error>(
                [&] { database.save_pending_delta(invalid); },
                "cannot save pending remote change"
            ) && database.pending_delta("me")->delta.delta_link == "fetched",
            "failed replacement did not roll back the previous snapshot"
        );
        invalid = expected;
        invalid.delta.changes.front().size = -1;
        require(
            test::throws_with<std::invalid_argument>(
                [&] { database.save_pending_delta(invalid); },
                "invalid remote metadata"
            ) && database.pending_delta("me")->delta.delta_link == "fetched",
            "invalid remote metadata destroyed the previous snapshot"
        );
        const auto path = temporary.path() / "items.sqlite3";
        require(
            execute_schema(
                path,
                "CREATE TRIGGER reject_pending BEFORE DELETE ON pending_delta "
                "BEGIN SELECT RAISE(ABORT, 'reject pending deletion'); END;"
            ),
            "cannot create commit-failure fixture"
        );
        require(
            test::throws_with<std::runtime_error>(
                [&] {
                    database.apply_delta(
                        {.drive_id = "me", .delta_link = "committed"}
                    );
                },
                "cannot clear pending delta snapshot"
            ) && database.delta_link("me") == "applied" &&
                database.pending_delta("me").has_value(),
            "applied cursor and pending cleanup were not atomic"
        );
        require(
            execute_schema(path, "DROP TRIGGER reject_pending;"),
            "cannot remove commit-failure fixture"
        );
        database.apply_delta({.drive_id = "me", .delta_link = "committed"});
        require(
            !database.pending_delta("me") &&
                database.delta_link("me") == "committed",
            "committing a batch retained its pending snapshot"
        );
        auto empty = expected;
        empty.applied_delta_link.reset();
        empty.applied_filter_fingerprint.reset();
        empty.delta.changes.clear();
        empty.apply_mode = storage::DeltaApplyMode::merge;
        database.save_pending_delta(empty);
        loaded = database.pending_delta("me");
        require(
            loaded && !loaded->applied_delta_link &&
                !loaded->applied_filter_fingerprint &&
                loaded->delta.changes.empty() &&
                loaded->apply_mode == storage::DeltaApplyMode::merge,
            "empty batch or null baseline was not preserved"
        );
        database.reset("me");
        require(
            !database.pending_delta("me"),
            "cursor reset retained a fetched cursor"
        );
        database.save_pending_delta(expected);
        require(
            database.clear("me").pending_deltas == 1 &&
                !database.pending_delta("me"),
            "full state clear retained pending remote metadata"
        );
    }
    {
        storage::ItemDatabase database{temporary.path(), identity()};
        database.open();
        database.save_pending_delta(expected);
    }
    {
        storage::ItemDatabase database{temporary.path(), identity()};
        database.open_read_only();
        require(
            database.pending_delta("me").has_value() &&
                test::throws_with<std::runtime_error>(
                    [&] { database.remove_pending_delta("me"); }, "readonly"
                ) &&
                database.pending_delta("me").has_value(),
            "read-only access changed pending state"
        );
    }
}

void test_version_26_upgrade() {
    test::TemporaryDirectory temporary;
    {
        storage::ItemDatabase database{temporary.path(), identity()};
        database.open();
        database.apply_delta({.drive_id = "me", .delta_link = "old-cursor"});
    }
    require(
        execute_schema(
            temporary.path() / "items.sqlite3",
            "DROP TABLE pending_delta_change; DROP TABLE pending_delta;"
            "PRAGMA user_version = 26;"
        ),
        "cannot create version 26 fixture"
    );
    storage::ItemDatabase database{temporary.path(), identity()};
    database.open();
    require(
        database.delta_link("me") == "old-cursor" &&
            !database.pending_delta("me") &&
            schema_version_is(temporary.path() / "items.sqlite3", 28),
        "version 26 upgrade lost the applied cursor"
    );
    database.save_pending_delta(sample());
}

void test_version_27_upgrade() {
    test::TemporaryDirectory temporary;
    auto expected = sample();
    {
        storage::ItemDatabase database{temporary.path(), identity()};
        database.open();
        database.apply_delta({.drive_id = "me", .delta_link = "applied"});
        database.save_pending_delta(expected);
    }
    require(
        execute_schema(
            temporary.path() / "items.sqlite3",
            "ALTER TABLE pending_delta_change DROP COLUMN unsupported_format;"
            "PRAGMA user_version = 27;"
        ),
        "cannot create version 27 fixture"
    );
    for (auto& item : expected.delta.changes) {
        item.unsupported_format.clear();
    }
    storage::ItemDatabase database{temporary.path(), identity()};
    database.open();
    const auto loaded = database.pending_delta("me");
    require(
        loaded &&
            loaded->delta.changes.size() == expected.delta.changes.size() &&
            database.delta_link("me") == "applied" &&
            schema_version_is(temporary.path() / "items.sqlite3", 28),
        "version 27 migration lost pending or applied metadata"
    );
    for (std::size_t index = 0; index < expected.delta.changes.size();
         ++index) {
        require(
            fields(loaded->delta.changes[index]) ==
                fields(expected.delta.changes[index]),
            "version 27 migration changed existing item metadata"
        );
    }
}

} // namespace

int main() {
    try {
        test_round_trip_and_transactions();
        test_version_26_upgrade();
        test_version_27_upgrade();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        return onedrive::test::fail(error.what());
    }
}
