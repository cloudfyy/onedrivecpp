#include "support.hpp"
#include "onedrive/storage/item_database.hpp"
#include "onedrive/storage/status.hpp"
#include "support/common.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace onedrive::test::db;
using onedrive::test::fail;
using onedrive::test::TemporaryDirectory;

int test_full_refresh_preserves_blocked_snapshots() {
    TemporaryDirectory temporary;
    onedrive::storage::ItemDatabase database{temporary.path(), identity()};
    database.open();
    const onedrive::storage::ItemState baseline{
        .drive_id = "me",
        .remote_id = "blocked",
        .etag = "trusted-etag",
        .remote_path = "old.txt",
        .local_path = temporary.path() / "old.txt",
        .local_size = 42,
        .local_modified_ticks = 123,
        .local_device = 1,
        .local_inode = 2,
        .content_hash = onedrive::util::FileHash{
            .algorithm = onedrive::util::FileHashAlgorithm::sha256,
            .value = "trusted-hash",
        },
    };
    database.upsert(baseline);
    database.upsert({.drive_id = "me", .remote_id = "discard"});
    database.upsert({.drive_id = "other", .remote_id = "blocked"});
    database.save_pending_move({
        .drive_id = "me",
        .remote_id = "blocked",
        .source_path = baseline.local_path,
        .destination_path = temporary.path() / "new.txt",
        .source_device = 1,
        .source_inode = 2,
    });
    database.apply_delta({
        .drive_id = "me",
        .blocked_upserts =
            {
                {.remote_id = "blocked",
                 .etag = "new-etag",
                 .remote_path = "new.txt",
                 .reason_code = "local_modification",
                 .reason_message = "local file changed"},
                {.remote_id = "untracked",
                 .reason_code = "malware_detected",
                 .reason_message = "remote file marked as malware"},
            },
        .delta_link = "refreshed",
        .apply_mode = onedrive::storage::DeltaApplyMode::replace,
    });
    const auto retained = database.find("me", "blocked");
    if (!retained || retained->etag != baseline.etag ||
        retained->local_path != baseline.local_path ||
        retained->local_size != baseline.local_size ||
        retained->local_modified_ticks != baseline.local_modified_ticks ||
        retained->local_device != 1 || retained->local_inode != 2 ||
        !retained->content_hash ||
        retained->content_hash->value != "trusted-hash" ||
        database.find("me", "discard") || database.find("me", "untracked") ||
        !database.find("other", "blocked") ||
        database.pending_moves("me").size() != 1 ||
        database.blocked_items("me").size() != 2) {
        return fail("full refresh did not preserve trusted blocked state");
    }
    try {
        database.apply_delta({
            .drive_id = "me",
            .blocked_upserts =
                {
                    {.remote_id = "blocked",
                     .reason_code = "local_modification",
                     .reason_message = "local file changed"},
                    {.remote_id = "invalid"},
                },
            .delta_link = "invalid",
            .apply_mode = onedrive::storage::DeltaApplyMode::replace,
        });
        return fail("invalid full refresh was accepted");
    } catch (const std::invalid_argument&) {
    }
    if (!database.find("me", "blocked") ||
        database.delta_link("me") != std::optional<std::string>{"refreshed"} ||
        database.blocked_items("me").size() != 2) {
        return fail("failed refresh did not roll back retained blocked state");
    }
    database.apply_delta({
        .drive_id = "me",
        .removals = {"blocked"},
        .blocked_removals = {"blocked"},
        .delta_link = "resolved",
    });
    if (database.find("me", "blocked") ||
        !database.pending_moves("me").empty() ||
        database.blocked_items("me").size() != 1) {
        return fail("resolved blocked snapshot or move journal was retained");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (const int result = test_full_refresh_preserves_blocked_snapshots();
        result != EXIT_SUCCESS) {
        return result;
    }
    TemporaryDirectory temporary_directory;
    const auto absent_summary = onedrive::storage::read_state_summary(
        temporary_directory.path() / "missing-drive",
        "me"
    );
    if (absent_summary.database_present) {
        return fail("missing state database was reported as available");
    }
    if (!onedrive::storage::diagnose_state_databases(
             temporary_directory.path() / "missing-state"
        )
             .empty()) {
        return fail("missing state directory produced database diagnostics");
    }

    {
        onedrive::storage::ItemDatabase database{
            temporary_directory.path(), identity()
        };
        database.open();
        const auto database_permissions =
            std::filesystem::status(
                temporary_directory.path() / "items.sqlite3"
            )
                .permissions();
        const auto directory_permissions =
            std::filesystem::status(temporary_directory.path()).permissions();
        if (database.size() != 0 ||
            (directory_permissions & std::filesystem::perms::all) !=
                std::filesystem::perms::owner_all ||
            (database_permissions & std::filesystem::perms::all) !=
                (std::filesystem::perms::owner_read |
                 std::filesystem::perms::owner_write)) {
            return fail("new state database was not created privately");
        }
        for (const auto* suffix : {"-wal", "-shm"}) {
            const auto sidecar = temporary_directory.path() /
                                 (std::string{"items.sqlite3"} + suffix);
            if (std::filesystem::exists(sidecar) &&
                (std::filesystem::status(sidecar).permissions() &
                 std::filesystem::perms::all) !=
                    (std::filesystem::perms::owner_read |
                     std::filesystem::perms::owner_write)) {
                return fail("SQLite sidecar file was not private");
            }
        }

        database.upsert({
            .remote_id = "remote-1",
            .etag = "etag-1",
            .local_path = "documents/report.txt",
        });
        database.upsert({
            .remote_id = "remote-2",
            .etag = "etag-2",
            .local_path = "photos/image.jpg",
        });
        database.upsert({
            .remote_id = "remote-1",
            .etag = "etag-updated",
            .local_path = "documents/report-renamed.txt",
        });
        database.apply_delta({
            .drive_id = "me",
            .upserts =
                {
                    {
                        .remote_id = "remote-3",
                        .parent_id = "root-id",
                        .name = "notes.txt",
                        .etag = "etag-3",
                        .remote_path = "notes.txt",
                        .local_path = temporary_directory.path() / "notes.txt",
                        .last_modified = "2026-10-02T00:00:00Z",
                        .size = 42,
                        .local_size = 42,
                        .local_modified_ticks = 123456,
                        .content_hash = onedrive::util::FileHash{
                            .algorithm =
                                onedrive::util::FileHashAlgorithm::sha256,
                            .value = "saved-hash",
                        },
                        .directory = false,
                    },
                },
            .delta_link = "https://graph.example.test/delta-1",
            .sync_filter_fingerprint = "filter-1",
        });

        if (database.size() != 3 ||
            database.delta_link("me") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-1"
                } ||
            database.sync_filter_fingerprint("me") !=
                std::optional<std::string>{"filter-1"}) {
            return fail("upsert did not preserve the expected item count");
        }
        const auto summary = onedrive::storage::read_state_summary(
            temporary_directory.path(),
            "me"
        );
        if (!summary.database_present || summary.tracked_items != 1 ||
            summary.blocked_items != 0 || !summary.delta_cursor ||
            summary.sync_filter_fingerprint != "filter-1") {
            return fail("read-only synchronization summary was incorrect");
        }
    }

    {
        onedrive::storage::ItemDatabase database{
            temporary_directory.path(), identity()
        };
        database.open_read_only();
        if (database.size() != 3 ||
            database.drive_items("me").size() != 1) {
            return fail("read-only database open did not expose saved state");
        }
        try {
            database.upsert({
                .remote_id = "read-only-write",
                .etag = "etag",
                .local_path = "read-only-write.txt",
            });
            return fail("read-only database open allowed a state write");
        } catch (const std::runtime_error&) {
        }
    }

    {
        TemporaryDirectory old_schema;
        const auto database_path = old_schema.path() / "items.sqlite3";
        if (!create_version_twenty_three_database(database_path)) {
            return fail("could not create old schema for read-only open");
        }
        try {
            onedrive::storage::ItemDatabase database{
                old_schema.path(), identity()
            };
            database.open_read_only();
            return fail("read-only database open migrated an old schema");
        } catch (const std::runtime_error&) {
        }
        if (!schema_version_is(database_path, 23)) {
            return fail("read-only database open changed the schema version");
        }
    }

    {
        onedrive::storage::ItemDatabase database{
            temporary_directory.path(), identity()
        };
        database.open();

        const auto first = database.find("", "remote-1");
        const auto second = database.find("", "remote-2");
        const auto third = database.find("me", "remote-3");
        const auto drive_items = database.drive_items("me");
        if (database.size() != 3 || !first || !second || !third) {
            return fail("persisted items were not loaded");
        }
        if (first->etag != "etag-updated" ||
            first->local_path != "documents/report-renamed.txt") {
            return fail("updated item state was not persisted");
        }
        if (second->etag != "etag-2" ||
            second->local_path != "photos/image.jpg") {
            return fail("second item state was not persisted");
        }
        if (third->drive_id != "me" || third->parent_id != "root-id" ||
            third->remote_path != "notes.txt" || third->size != 42 ||
            third->local_size != 42 || third->local_modified_ticks != 123456 ||
            !third->content_hash ||
            third->content_hash->algorithm !=
                onedrive::util::FileHashAlgorithm::sha256 ||
            third->content_hash->value != "saved-hash" ||
            third->directory ||
            database.delta_link("me") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-1"
                } ||
            database.sync_filter_fingerprint("me") !=
                std::optional<std::string>{"filter-1"} ||
            drive_items.size() != 1 || drive_items[0].remote_id != "remote-3") {
            return fail("delta item state was not persisted");
        }

        try {
            database.apply_delta({
                .drive_id = "me",
                .upserts =
                    {
                        {
                            .remote_id = "rolled-back",
                            .name = "rolled-back.txt",
                            .etag = "rolled-back-etag",
                            .remote_path = "rolled-back.txt",
                            .local_path =
                                temporary_directory.path() / "rolled-back.txt",
                        },
                    },
                .removals = {""},
                .delta_link = "https://graph.example.test/delta-invalid",
                .sync_filter_fingerprint = "filter-invalid",
            });
            return fail("invalid delta state was accepted");
        } catch (const std::invalid_argument&) {
        }
        if (database.find("me", "rolled-back") ||
            database.delta_link("me") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-1"
                } ||
            database.sync_filter_fingerprint("me") !=
                std::optional<std::string>{"filter-1"}) {
            return fail("failed delta update was not rolled back");
        }

        database.apply_delta({
            .drive_id = "me",
            .removals = {"remote-3"},
            .delta_link = "https://graph.example.test/delta-2",
            .sync_filter_fingerprint = "filter-1",
        });
        if (database.size() != 2 || database.find("me", "remote-3") ||
            database.delta_link("me") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-2"
                } ||
            database.sync_filter_fingerprint("me") !=
                std::optional<std::string>{"filter-1"}) {
            return fail("delta removal was not persisted");
        }
    }
    if (!std::filesystem::exists(
            temporary_directory.path() / "items.sqlite3"
        )) {
        return fail("SQLite state database was not created");
    }
    if (!identity_row_is_valid(temporary_directory.path() / "items.sqlite3")) {
        return fail("account identity and avatar were not saved");
    }
    auto mismatched_identity = identity();
    mismatched_identity.user_id = "different-user";
    try {
        onedrive::storage::ItemDatabase database{
            temporary_directory.path(), std::move(mismatched_identity)
        };
        database.open();
        return fail("mismatched account identity was accepted");
    } catch (const std::runtime_error&) {
    }

    const auto concurrent_directory = temporary_directory.path() / "concurrent";
    {
        onedrive::storage::ItemDatabase database{
            concurrent_directory, identity()
        };
        try {
            static_cast<void>(database.size());
            return fail("unopened database query did not propagate its error");
        } catch (const std::runtime_error&) {
        }
        database.open();

        constexpr std::size_t thread_count = 8;
        constexpr std::size_t items_per_thread = 50;
        std::atomic_bool succeeded{true};
        std::vector<std::jthread> workers;
        workers.reserve(thread_count);
        for (std::size_t thread = 0; thread < thread_count; ++thread) {
            workers.emplace_back([&, thread] {
                try {
                    for (std::size_t index = 0; index < items_per_thread;
                         ++index) {
                        const auto remote_id = "thread-" +
                                               std::to_string(thread) +
                                               "-item-" + std::to_string(index);
                        database.upsert({
                            .drive_id = "concurrent-drive",
                            .remote_id = remote_id,
                            .etag = "etag-" + std::to_string(index),
                            .local_path = remote_id,
                        });
                        const auto stored =
                            database.find("concurrent-drive", remote_id);
                        if (!stored || stored->remote_id != remote_id) {
                            succeeded = false;
                            return;
                        }
                    }
                } catch (...) {
                    succeeded = false;
                }
            });
        }
        workers.clear();

        if (!succeeded || database.size() != thread_count * items_per_thread) {
            return fail("concurrent ItemDatabase access was not serialized");
        }
    }

    const auto partial_directory =
        temporary_directory.path() / "partial-download";
    const auto partial_path =
        partial_directory / ".resume.txt.onedrive-partial-test";
    {
        onedrive::storage::ItemDatabase database{partial_directory, identity()};
        database.open();
        database.save_partial_download({
            .item =
                {
                    .drive_id = "me",
                    .remote_id = "resume",
                    .parent_id = "root",
                    .name = "resume.txt",
                    .etag = "resume-etag",
                    .remote_path = "resume.txt",
                    .local_path = partial_directory / "resume.txt",
                    .last_modified = "2026-10-03T00:00:00Z",
                    .size = 8,
                },
            .temporary_path = partial_path,
            .completed_bytes = 4,
        });
    }
    {
        onedrive::storage::ItemDatabase database{partial_directory, identity()};
        database.open();
        const auto partial = database.partial_download("me", "resume");
        if (!partial || partial->item.etag != "resume-etag" ||
            partial->temporary_path != partial_path ||
            partial->completed_bytes != 4) {
            return fail("partial download state was not persisted");
        }
        database.remove_partial_download("me", "resume");
        if (database.partial_download("me", "resume")) {
            return fail("partial download state was not removed");
        }
    }

    return EXIT_SUCCESS;
}
