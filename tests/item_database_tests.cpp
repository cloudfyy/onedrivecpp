#include "onedrive/storage/item_database.hpp"
#include "test_support.hpp"

#include <sqlite3.h>

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using onedrive::test::TemporaryDirectory;

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

onedrive::account::DriveIdentity identity() {
    return {
        .user_id = "user-id",
        .user_display_name = "Test User",
        .configured_drive_id = "me",
        .drive_id = "canonical-drive-id",
        .drive_name = "Test Drive",
        .photo = onedrive::account::ProfilePhoto{
            .content_type = "image/jpeg",
            .bytes = {1, 2, 3, 4},
        },
    };
}

bool create_version_four_database(const std::filesystem::path& path) {
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    constexpr const char* schema =
        "CREATE TABLE item ("
        "drive_id TEXT NOT NULL, remote_id TEXT NOT NULL, "
        "parent_id TEXT NOT NULL, name TEXT NOT NULL, etag TEXT NOT NULL, "
        "remote_path TEXT NOT NULL, local_path TEXT NOT NULL, "
        "last_modified TEXT NOT NULL, size INTEGER NOT NULL, "
        "local_size INTEGER NOT NULL, local_modified_ticks INTEGER NOT NULL, "
        "directory INTEGER NOT NULL, PRIMARY KEY (drive_id, remote_id));"
        "CREATE TABLE drive_state ("
        "drive_id TEXT PRIMARY KEY NOT NULL, delta_link TEXT NOT NULL);"
        "CREATE TABLE pending_download ("
        "drive_id TEXT NOT NULL, remote_id TEXT NOT NULL, "
        "parent_id TEXT NOT NULL, name TEXT NOT NULL, etag TEXT NOT NULL, "
        "remote_path TEXT NOT NULL, local_path TEXT NOT NULL, "
        "last_modified TEXT NOT NULL, size INTEGER NOT NULL, "
        "directory INTEGER NOT NULL, temporary_path TEXT NOT NULL, "
        "content_fingerprint TEXT NOT NULL, "
        "PRIMARY KEY (drive_id, remote_id));"
        "PRAGMA user_version = 4;";
    const bool succeeded =
        sqlite3_exec(database, schema, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(database);
    return succeeded;
}

bool create_version_six_database(const std::filesystem::path& path) {
    if (!create_version_four_database(path)) {
        return false;
    }
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    constexpr const char* schema =
        "CREATE TABLE blocked_item ("
        "drive_id TEXT NOT NULL, remote_id TEXT NOT NULL, "
        "parent_id TEXT NOT NULL, name TEXT NOT NULL, etag TEXT NOT NULL, "
        "remote_path TEXT NOT NULL, last_modified TEXT NOT NULL, "
        "size INTEGER NOT NULL, directory INTEGER NOT NULL, "
        "reason_code TEXT NOT NULL, reason_message TEXT NOT NULL, "
        "first_seen INTEGER NOT NULL DEFAULT (unixepoch()), "
        "last_attempt INTEGER NOT NULL DEFAULT (unixepoch()), "
        "attempt_count INTEGER NOT NULL DEFAULT 1, "
        "PRIMARY KEY (drive_id, remote_id));"
        "CREATE TABLE identity ("
        "singleton INTEGER PRIMARY KEY NOT NULL CHECK (singleton = 1), "
        "user_id TEXT NOT NULL, user_display_name TEXT NOT NULL, "
        "drive_id TEXT NOT NULL, drive_name TEXT NOT NULL, "
        "avatar_content_type TEXT NOT NULL, avatar_bytes BLOB NOT NULL);"
        "PRAGMA user_version = 6;";
    const bool succeeded =
        sqlite3_exec(database, schema, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(database);
    return succeeded;
}

bool create_version_seven_database(const std::filesystem::path& path) {
    if (!create_version_six_database(path)) {
        return false;
    }
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    constexpr const char* schema =
        "CREATE TABLE drive_mapping ("
        "configured_drive_id TEXT PRIMARY KEY NOT NULL, "
        "canonical_drive_id TEXT NOT NULL, "
        "last_resolved INTEGER NOT NULL DEFAULT (unixepoch()));"
        "PRAGMA user_version = 7;";
    const bool succeeded =
        sqlite3_exec(database, schema, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(database);
    return succeeded;
}

bool create_version_eight_database(const std::filesystem::path& path) {
    if (!create_version_seven_database(path)) {
        return false;
    }

    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    constexpr const char* schema =
        "CREATE TABLE partial_download ("
        "drive_id TEXT NOT NULL, remote_id TEXT NOT NULL, "
        "parent_id TEXT NOT NULL, name TEXT NOT NULL, etag TEXT NOT NULL, "
        "remote_path TEXT NOT NULL, local_path TEXT NOT NULL, "
        "last_modified TEXT NOT NULL, size INTEGER NOT NULL, "
        "directory INTEGER NOT NULL, temporary_path TEXT NOT NULL, "
        "completed_bytes INTEGER NOT NULL, "
        "updated_at INTEGER NOT NULL DEFAULT (unixepoch()), "
        "PRIMARY KEY (drive_id, remote_id));"
        "PRAGMA user_version = 8;";
    const bool succeeded =
        sqlite3_exec(database, schema, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(database);
    return succeeded;
}

bool create_version_nine_database(const std::filesystem::path& path) {
    if (!create_version_eight_database(path)) {
        return false;
    }
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    constexpr const char* schema =
        "ALTER TABLE blocked_item ADD COLUMN content_hash_algorithm "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE blocked_item ADD COLUMN content_hash_value "
        "TEXT NOT NULL DEFAULT '';"
        "PRAGMA user_version = 9;";
    const bool succeeded =
        sqlite3_exec(database, schema, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(database);
    return succeeded;
}

bool identity_row_is_valid(const std::filesystem::path& path) {
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const bool prepared = sqlite3_prepare_v2(
        database,
        "SELECT user_id, user_display_name, drive_id, drive_name, "
        "avatar_content_type, length(avatar_bytes) FROM identity "
        "WHERE singleton = 1;",
        -1,
        &statement,
        nullptr
    ) == SQLITE_OK;
    const bool valid =
        prepared && sqlite3_step(statement) == SQLITE_ROW &&
        std::string{
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))
        } == "user-id" &&
        std::string{
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 1))
        } == "Test User" &&
        std::string{
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 2))
        } == "canonical-drive-id" &&
        std::string{
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 3))
        } == "Test Drive" &&
        std::string{
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 4))
        } == "image/jpeg" &&
        sqlite3_column_int(statement, 5) == 4;
    sqlite3_finalize(statement);
    statement = nullptr;
    const bool mapping_prepared = sqlite3_prepare_v2(
        database,
        "SELECT canonical_drive_id FROM drive_mapping "
        "WHERE configured_drive_id = 'me';",
        -1,
        &statement,
        nullptr
    ) == SQLITE_OK;
    const bool mapping_valid =
        mapping_prepared && sqlite3_step(statement) == SQLITE_ROW &&
        std::string{
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))
        } == "canonical-drive-id";
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return valid && mapping_valid;
}

}  // namespace

int main() {
    TemporaryDirectory temporary_directory;

    {
        onedrive::storage::ItemDatabase database{
            temporary_directory.path(),
            identity()
        };
        database.open();
        const auto database_permissions =
            std::filesystem::status(
                temporary_directory.path() / "items.sqlite3"
            ).permissions();
        if (database.size() != 0 ||
            (database_permissions & std::filesystem::perms::all) !=
                (std::filesystem::perms::owner_read |
                 std::filesystem::perms::owner_write)) {
            return fail("new state database is not empty");
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
            .upserts = {
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
    }

    {
        onedrive::storage::ItemDatabase database{
            temporary_directory.path(),
            identity()
        };
        database.open();

        const auto first = database.find("", "remote-1");
        const auto second = database.find("", "remote-2");
        const auto third = database.find("me", "remote-3");
        if (database.size() != 3 || !first || !second || !third) {
            return fail("persisted items were not loaded");
        }
        if (first->etag != "etag-updated" ||
            first->local_path != "documents/report-renamed.txt") {
            return fail("updated item state was not persisted");
        }
        if (second->etag != "etag-2" || second->local_path != "photos/image.jpg") {
            return fail("second item state was not persisted");
        }
        if (third->drive_id != "me" || third->parent_id != "root-id" ||
            third->remote_path != "notes.txt" || third->size != 42 ||
            third->local_size != 42 ||
            third->local_modified_ticks != 123456 || third->directory ||
            database.delta_link("me") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-1"
                } ||
            database.sync_filter_fingerprint("me") !=
                std::optional<std::string>{"filter-1"}) {
            return fail("delta item state was not persisted");
        }

        try {
            database.apply_delta({
                .drive_id = "me",
                .upserts = {
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

    {
        onedrive::storage::ItemDatabase database{
            temporary_directory.path(),
            identity()
        };
        database.open();
        if (database.size() != 2 || database.find("me", "remote-3") ||
            database.delta_link("me") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-2"
                }) {
            return fail("updated delta state was not loaded");
        }

        database.apply_delta({
            .drive_id = "me",
            .upserts = {
                {
                    .remote_id = "reset-me",
                    .name = "reset-me.txt",
                    .etag = "reset-me-etag",
                    .remote_path = "reset-me.txt",
                    .local_path = temporary_directory.path() / "reset-me.txt",
                },
            },
            .blocked_upserts = {
                {
                    .remote_id = "blocked-me",
                    .name = "blocked-me.txt",
                    .etag = "blocked-etag",
                    .remote_path = "blocked-me.txt",
                    .reason_code = "local_modification",
                    .reason_message = "local file was modified",
                    .content_hash = onedrive::FileHash{
                        .algorithm = onedrive::FileHashAlgorithm::quick_xor,
                        .value = "SgAAAAAAAAAAAAAAAQAAAAAAAAA=",
                    },
                },
            },
            .delta_link = "https://graph.example.test/delta-me",
        });
        database.apply_delta({
            .drive_id = "other-drive",
            .upserts = {
                {
                    .remote_id = "keep-me",
                    .name = "keep-me.txt",
                    .etag = "keep-me-etag",
                    .remote_path = "keep-me.txt",
                    .local_path = temporary_directory.path() / "keep-me.txt",
                },
            },
            .blocked_upserts = {
                {
                    .remote_id = "blocked-other",
                    .name = "blocked-other.txt",
                    .etag = "blocked-etag",
                    .remote_path = "blocked-other.txt",
                    .reason_code = "invalid_remote_path",
                    .reason_message = "invalid name",
                },
            },
            .delta_link = "https://graph.example.test/delta-other",
        });
        database.save_pending_download({
            .item = {
                .drive_id = "me",
                .remote_id = "pending-me",
                .name = "pending-me.txt",
                .etag = "pending-etag",
                .remote_path = "pending-me.txt",
                .local_path = temporary_directory.path() / "pending-me.txt",
                .size = 4,
            },
            .temporary_path = temporary_directory.path() / "pending-me.tmp",
            .content_fingerprint = "fingerprint-me",
        });
        database.save_pending_download({
            .item = {
                .drive_id = "other-drive",
                .remote_id = "pending-other",
                .name = "pending-other.txt",
                .etag = "pending-etag",
                .remote_path = "pending-other.txt",
                .local_path = temporary_directory.path() / "pending-other.txt",
                .size = 5,
            },
            .temporary_path = temporary_directory.path() / "pending-other.tmp",
            .content_fingerprint = "fingerprint-other",
        });
        database.save_partial_download({
            .item = {
                .drive_id = "me",
                .remote_id = "partial-me",
                .name = "partial-me.txt",
                .etag = "partial-etag",
                .remote_path = "partial-me.txt",
                .local_path = temporary_directory.path() / "partial-me.txt",
                .size = 4,
            },
            .temporary_path =
                temporary_directory.path() / ".partial-me.tmp",
            .completed_bytes = 2,
        });
        database.save_partial_download({
            .item = {
                .drive_id = "other-drive",
                .remote_id = "partial-other",
                .name = "partial-other.txt",
                .etag = "partial-etag",
                .remote_path = "partial-other.txt",
                .local_path =
                    temporary_directory.path() / "partial-other.txt",
                .size = 5,
            },
            .temporary_path =
                temporary_directory.path() / ".partial-other.tmp",
            .completed_bytes = 3,
        });
        if (database.pending_downloads("me").size() != 1 ||
            database.pending_downloads("other-drive").size() != 1 ||
            !database.partial_download("me", "partial-me") ||
            !database.partial_download("other-drive", "partial-other") ||
            database.blocked_items("me").size() != 1 ||
            database.blocked_items("other-drive").size() != 1 ||
            database.blocked_items("me")[0].attempt_count != 1 ||
            !database.blocked_items("me")[0].content_hash ||
            database.blocked_items("me")[0].content_hash->value !=
                "SgAAAAAAAAAAAAAAAQAAAAAAAAA=") {
            return fail("pending downloads or blocked items were not saved by drive");
        }

        if (!database.reset("me") || database.size() != 4 ||
            !database.find("me", "reset-me") ||
            !database.find("other-drive", "keep-me") ||
            database.delta_link("me").has_value() ||
            database.pending_downloads("me").size() != 1 ||
            database.pending_downloads("other-drive").size() != 1 ||
            !database.partial_download("me", "partial-me") ||
            !database.partial_download("other-drive", "partial-other") ||
            database.blocked_items("me").size() != 1 ||
            database.blocked_items("other-drive").size() != 1 ||
            database.delta_link("other-drive") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-other"
                }) {
            return fail("cursor reset did not preserve recovery state");
        }
        if (database.reset("me")) {
            return fail("resetting an absent cursor reported a removal");
        }
    }

    {
        onedrive::storage::ItemDatabase database{
            temporary_directory.path(),
            identity()
        };
        database.open();
        if (database.size() != 4 ||
            !database.find("me", "reset-me") ||
            !database.find("other-drive", "keep-me") ||
            database.delta_link("me").has_value() ||
            database.pending_downloads("me").size() != 1 ||
            database.pending_downloads("other-drive").size() != 1 ||
            !database.partial_download("me", "partial-me") ||
            !database.partial_download("other-drive", "partial-other") ||
            database.blocked_items("me").size() != 1 ||
            database.blocked_items("other-drive").size() != 1 ||
            database.delta_link("other-drive") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-other"
                }) {
            return fail("safe cursor reset was not persisted");
        }

        database.apply_delta({
            .drive_id = "me",
            .upserts = {
                {
                    .remote_id = "fresh-me",
                    .name = "fresh-me.txt",
                    .etag = "fresh-me-etag",
                    .remote_path = "fresh-me.txt",
                    .local_path = temporary_directory.path() / "fresh-me.txt",
                },
            },
            .blocked_upserts = {
                {
                    .remote_id = "fresh-blocked-me",
                    .name = "fresh-blocked-me.txt",
                    .etag = "fresh-blocked-etag",
                    .remote_path = "fresh-blocked-me.txt",
                    .reason_code = "local_modification",
                    .reason_message = "still modified",
                },
            },
            .delta_link = "https://graph.example.test/delta-fresh",
            .replace_drive_items = true,
        });
        if (database.size() != 4 ||
            database.find("me", "reset-me") ||
            !database.find("me", "fresh-me") ||
            !database.find("other-drive", "keep-me") ||
            !database.find("", "remote-1") ||
            !database.find("", "remote-2") ||
            database.pending_downloads("me").size() != 1 ||
            database.pending_downloads("other-drive").size() != 1 ||
            !database.partial_download("me", "partial-me") ||
            !database.partial_download("other-drive", "partial-other") ||
            database.blocked_items("me").size() != 1 ||
            database.blocked_items("me")[0].remote_id != "fresh-blocked-me" ||
            database.blocked_items("other-drive").size() != 1) {
            return fail("initial delta did not replace only the selected drive");
        }

        const auto cleared = database.clear("me");
        if (cleared.items != 1 || cleared.pending_downloads != 1 ||
            cleared.partial_downloads != 1 ||
            cleared.blocked_items != 1 || !cleared.delta_link ||
            database.size() != 3 ||
            database.find("me", "fresh-me") ||
            !database.find("other-drive", "keep-me") ||
            !database.find("", "remote-1") ||
            !database.find("", "remote-2") ||
            !database.pending_downloads("me").empty() ||
            database.pending_downloads("other-drive").size() != 1 ||
            database.partial_download("me", "partial-me") ||
            !database.partial_download("other-drive", "partial-other") ||
            !database.blocked_items("me").empty() ||
            database.blocked_items("other-drive").size() != 1 ||
            database.delta_link("me").has_value() ||
            database.delta_link("other-drive") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-other"
                }) {
            return fail("full clear did not isolate the configured drive");
        }
    }

    if (!std::filesystem::exists(temporary_directory.path() / "items.sqlite3")) {
        return fail("SQLite state database was not created");
    }
    if (!identity_row_is_valid(
            temporary_directory.path() / "items.sqlite3"
        )) {
        return fail("account identity and avatar were not saved");
    }
    auto mismatched_identity = identity();
    mismatched_identity.user_id = "different-user";
    try {
        onedrive::storage::ItemDatabase database{
            temporary_directory.path(),
            std::move(mismatched_identity)
        };
        database.open();
        return fail("mismatched account identity was accepted");
    } catch (const std::runtime_error&) {
    }

    const auto concurrent_directory =
        temporary_directory.path() / "concurrent";
    {
        onedrive::storage::ItemDatabase database{
            concurrent_directory,
            identity()
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
                    for (std::size_t index = 0;
                         index < items_per_thread;
                         ++index) {
                        const auto remote_id =
                            "thread-" + std::to_string(thread) + "-item-" +
                            std::to_string(index);
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

        if (!succeeded ||
            database.size() != thread_count * items_per_thread) {
            return fail("concurrent ItemDatabase access was not serialized");
        }
    }

    const auto partial_directory =
        temporary_directory.path() / "partial-download";
    const auto partial_path =
        partial_directory / ".resume.txt.onedrive-partial-test";
    {
        onedrive::storage::ItemDatabase database{
            partial_directory,
            identity()
        };
        database.open();
        database.save_partial_download({
            .item = {
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
        onedrive::storage::ItemDatabase database{
            partial_directory,
            identity()
        };
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

    const auto migration_directory =
        temporary_directory.path() / "version-four";
    std::filesystem::create_directories(migration_directory);
    if (!create_version_four_database(
            migration_directory / "items.sqlite3"
        )) {
        return fail("version four migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            migration_directory,
            identity()
        };
        database.open();
        database.apply_delta({
            .drive_id = "me",
            .blocked_upserts = {
                {
                    .remote_id = "migrated-blocked",
                    .name = "blocked.txt",
                    .etag = "etag",
                    .remote_path = "blocked.txt",
                    .reason_code = "local_modification",
                    .reason_message = "local file was modified",
                },
            },
            .delta_link = "https://graph.example.test/migrated",
        });
        if (database.blocked_items("me").size() != 1) {
            return fail("version four database was not migrated to blocked items");
        }
    }
    {
        onedrive::storage::ItemDatabase database{
            migration_directory,
            identity()
        };
        database.open();
        if (database.blocked_items("me").size() != 1 ||
            database.blocked_items("me")[0].remote_id !=
                "migrated-blocked") {
            return fail("migrated blocked item was not persisted");
        }
    }

    const auto version_six_directory =
        temporary_directory.path() / "version-six";
    std::filesystem::create_directories(version_six_directory);
    if (!create_version_six_database(
            version_six_directory / "items.sqlite3"
        )) {
        return fail("version six migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_six_directory,
            identity()
        };
        database.open();
    }
    if (!identity_row_is_valid(
            version_six_directory / "items.sqlite3"
        )) {
        return fail("version six database did not gain the Drive ID mapping");
    }

    const auto version_seven_directory =
        temporary_directory.path() / "version-seven";
    std::filesystem::create_directories(version_seven_directory);
    if (!create_version_seven_database(
            version_seven_directory / "items.sqlite3"
        )) {
        return fail("version seven migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_seven_directory,
            identity()
        };
        database.open();
        database.save_partial_download({
            .item = {
                .drive_id = "me",
                .remote_id = "migrated-partial",
                .name = "migrated-partial.txt",
                .etag = "etag",
                .remote_path = "migrated-partial.txt",
                .local_path =
                    version_seven_directory / "migrated-partial.txt",
                .size = 4,
            },
            .temporary_path =
                version_seven_directory / ".migrated-partial.tmp",
            .completed_bytes = 2,
        });
        if (!database.partial_download("me", "migrated-partial")) {
            return fail(
                "version seven database did not gain partial download state"
            );
        }
    }

    const auto version_eight_directory =
        temporary_directory.path() / "version-eight";
    std::filesystem::create_directories(version_eight_directory);
    if (!create_version_eight_database(
            version_eight_directory / "items.sqlite3"
        )) {
        return fail("version eight migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_eight_directory,
            identity()
        };
        database.open();
        database.apply_delta({
            .drive_id = "me",
            .blocked_upserts = {
                {
                    .remote_id = "hash-after-migration",
                    .name = "hash.txt",
                    .etag = "etag",
                    .remote_path = "hash.txt",
                    .reason_code = "local_modification",
                    .reason_message = "local file was modified",
                    .content_hash = onedrive::FileHash{
                        .algorithm = onedrive::FileHashAlgorithm::sha256,
                        .value =
                            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                            "AAAAAAAAAAAAAAAA",
                    },
                },
            },
            .delta_link = "https://graph.example.test/delta-v9",
            .sync_filter_fingerprint = "migrated-filter",
        });
        const auto blocked = database.blocked_items("me");
        if (blocked.size() != 1 || !blocked[0].content_hash ||
            blocked[0].content_hash->algorithm !=
                onedrive::FileHashAlgorithm::sha256 ||
            database.sync_filter_fingerprint("me") !=
                std::optional<std::string>{"migrated-filter"}) {
            return fail(
                "version eight database did not gain blocked hash metadata"
            );
        }
    }

    const auto version_nine_directory =
        temporary_directory.path() / "version-nine";
    std::filesystem::create_directories(version_nine_directory);
    if (!create_version_nine_database(
            version_nine_directory / "items.sqlite3"
        )) {
        return fail("version nine migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_nine_directory,
            identity()
        };
        database.open();
        database.apply_delta({
            .drive_id = "me",
            .delta_link = "https://graph.example.test/delta-v10",
            .sync_filter_fingerprint = "version-nine-filter",
        });
        if (database.sync_filter_fingerprint("me") !=
                std::optional<std::string>{"version-nine-filter"}) {
            return fail(
                "version nine database did not gain selective sync state"
            );
        }
    }

    return EXIT_SUCCESS;
}
