#include "onedrive/storage/item_database.hpp"
#include "test_support.hpp"

#include <sqlite3.h>

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using onedrive::test::TemporaryDirectory;
using onedrive::test::fail;

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

bool execute_schema(
    const std::filesystem::path& path,
    const char* schema
) {
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    const bool succeeded =
        sqlite3_exec(database, schema, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(database);
    return succeeded;
}

bool create_version_one_database(const std::filesystem::path& path) {
    return execute_schema(
        path,
        "CREATE TABLE item ("
        "remote_id TEXT PRIMARY KEY NOT NULL, etag TEXT NOT NULL, "
        "local_path TEXT NOT NULL);"
        "PRAGMA user_version = 1;"
    );
}

bool create_version_two_database(const std::filesystem::path& path) {
    return execute_schema(
        path,
        "CREATE TABLE item ("
        "drive_id TEXT NOT NULL, remote_id TEXT NOT NULL, "
        "parent_id TEXT NOT NULL, name TEXT NOT NULL, etag TEXT NOT NULL, "
        "remote_path TEXT NOT NULL, local_path TEXT NOT NULL, "
        "last_modified TEXT NOT NULL, size INTEGER NOT NULL, "
        "directory INTEGER NOT NULL, PRIMARY KEY (drive_id, remote_id));"
        "CREATE TABLE drive_state ("
        "drive_id TEXT PRIMARY KEY NOT NULL, delta_link TEXT NOT NULL);"
        "PRAGMA user_version = 2;"
    );
}

bool create_version_three_database(const std::filesystem::path& path) {
    if (!create_version_two_database(path)) {
        return false;
    }
    return execute_schema(
        path,
        "ALTER TABLE item ADD COLUMN local_size INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE item ADD COLUMN local_modified_ticks INTEGER NOT NULL "
        "DEFAULT 0;"
        "PRAGMA user_version = 3;"
    );
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

bool create_version_five_database(const std::filesystem::path& path) {
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
        "PRAGMA user_version = 5;";
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

bool create_version_ten_database(const std::filesystem::path& path) {
    if (!create_version_nine_database(path)) {
        return false;
    }
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    constexpr const char* schema =
        "ALTER TABLE drive_state ADD COLUMN sync_filter_fingerprint "
        "TEXT NOT NULL DEFAULT '';"
        "PRAGMA user_version = 10;";
    const bool succeeded =
        sqlite3_exec(database, schema, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(database);
    return succeeded;
}

bool create_version_eleven_database(const std::filesystem::path& path) {
    if (!create_version_ten_database(path)) {
        return false;
    }
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    constexpr const char* schema =
        "ALTER TABLE pending_download ADD COLUMN backup_path "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE pending_download ADD COLUMN backup_fingerprint "
        "TEXT NOT NULL DEFAULT '';"
        "PRAGMA user_version = 11;";
    const bool succeeded =
        sqlite3_exec(database, schema, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(database);
    return succeeded;
}

bool create_version_twelve_database(const std::filesystem::path& path) {
    if (!create_version_eleven_database(path)) {
        return false;
    }
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    constexpr const char* schema =
        "ALTER TABLE blocked_item ADD COLUMN deleted "
        "INTEGER NOT NULL DEFAULT 0;"
        "PRAGMA user_version = 12;";
    const bool succeeded =
        sqlite3_exec(database, schema, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(database);
    return succeeded;
}

bool create_version_thirteen_database(const std::filesystem::path& path) {
    if (!create_version_twelve_database(path)) {
        return false;
    }
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    constexpr const char* schema =
        "CREATE TABLE pending_upload ("
        "drive_id TEXT NOT NULL, remote_path TEXT NOT NULL, "
        "local_path TEXT NOT NULL, snapshot_path TEXT NOT NULL, "
        "content_fingerprint TEXT NOT NULL, local_size INTEGER NOT NULL, "
        "local_modified_ticks INTEGER NOT NULL, "
        "remote_id TEXT NOT NULL DEFAULT '', "
        "expected_etag TEXT NOT NULL DEFAULT '', "
        "PRIMARY KEY (drive_id, remote_path));"
        "PRAGMA user_version = 13;";
    const bool succeeded =
        sqlite3_exec(database, schema, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(database);
    return succeeded;
}

bool create_version_fourteen_database(const std::filesystem::path& path) {
    if (!create_version_thirteen_database(path)) {
        return false;
    }
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    constexpr const char* schema =
        "CREATE TABLE pending_move ("
        "drive_id TEXT NOT NULL, remote_id TEXT NOT NULL, "
        "source_path TEXT NOT NULL, destination_path TEXT NOT NULL, "
        "source_device INTEGER NOT NULL, source_inode INTEGER NOT NULL, "
        "directory INTEGER NOT NULL, "
        "PRIMARY KEY (drive_id, remote_id));"
        "INSERT INTO pending_move ("
        "drive_id, remote_id, source_path, destination_path, "
        "source_device, source_inode, directory"
        ") VALUES ('me', 'legacy-move', '/sync/old', '/sync/new', "
        "123, 456, 0);"
        "PRAGMA user_version = 14;";
    const bool succeeded =
        sqlite3_exec(database, schema, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(database);
    return succeeded;
}

bool create_version_fifteen_database(const std::filesystem::path& path) {
    if (!create_version_fourteen_database(path)) {
        return false;
    }
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    constexpr const char* migration =
        "ALTER TABLE pending_move ADD COLUMN staging_path "
        "TEXT NOT NULL DEFAULT '';"
        "PRAGMA user_version = 15;";
    const bool succeeded =
        sqlite3_exec(
            database,
            migration,
            nullptr,
            nullptr,
            nullptr
        ) == SQLITE_OK;
    sqlite3_close(database);
    return succeeded;
}

bool create_version_sixteen_database(const std::filesystem::path& path) {
    if (!create_version_fifteen_database(path)) {
        return false;
    }
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    constexpr const char* migration =
        "CREATE TABLE upload_suppression ("
        "drive_id TEXT NOT NULL, remote_id TEXT NOT NULL, "
        "local_path TEXT NOT NULL, source_device INTEGER NOT NULL, "
        "source_inode INTEGER NOT NULL, "
        "PRIMARY KEY (drive_id, local_path));"
        "PRAGMA user_version = 16;";
    const bool succeeded =
        sqlite3_exec(
            database,
            migration,
            nullptr,
            nullptr,
            nullptr
        ) == SQLITE_OK;
    sqlite3_close(database);
    return succeeded;
}

bool create_version_seventeen_database(const std::filesystem::path& path) {
    if (!create_version_sixteen_database(path)) {
        return false;
    }
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    constexpr const char* migration =
        "ALTER TABLE pending_upload ADD COLUMN upload_url "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE pending_upload ADD COLUMN upload_expiration "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE pending_upload ADD COLUMN completed_bytes "
        "INTEGER NOT NULL DEFAULT 0;"
        "PRAGMA user_version = 17;";
    const bool succeeded =
        sqlite3_exec(
            database,
            migration,
            nullptr,
            nullptr,
            nullptr
        ) == SQLITE_OK;
    sqlite3_close(database);
    return succeeded;
}

bool create_version_eighteen_database(const std::filesystem::path& path) {
    if (!create_version_seventeen_database(path)) {
        return false;
    }
    return execute_schema(
        path,
        "ALTER TABLE pending_upload ADD COLUMN directory "
        "INTEGER NOT NULL DEFAULT 0;"
        "PRAGMA user_version = 18;"
    );
}

bool create_version_nineteen_database(const std::filesystem::path& path) {
    if (!create_version_eighteen_database(path)) {
        return false;
    }
    return execute_schema(
        path,
        "CREATE TABLE pending_delete ("
        "drive_id TEXT NOT NULL, remote_id TEXT NOT NULL, "
        "expected_etag TEXT NOT NULL, remote_path TEXT NOT NULL, "
        "local_path TEXT NOT NULL, directory INTEGER NOT NULL, "
        "PRIMARY KEY (drive_id, remote_id));"
        "PRAGMA user_version = 19;"
    );
}

bool create_version_twenty_database(const std::filesystem::path& path) {
    if (!create_version_nineteen_database(path)) {
        return false;
    }
    return execute_schema(
        path,
        "ALTER TABLE item ADD COLUMN local_device "
        "INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE item ADD COLUMN local_inode "
        "INTEGER NOT NULL DEFAULT 0;"
        "PRAGMA user_version = 20;"
    );
}

bool create_version_twenty_one_database(
    const std::filesystem::path& path
) {
    if (!create_version_twenty_database(path)) {
        return false;
    }
    return execute_schema(
        path,
        "CREATE TABLE pending_remote_move ("
        "drive_id TEXT NOT NULL, remote_id TEXT NOT NULL, "
        "expected_etag TEXT NOT NULL, source_remote_path TEXT NOT NULL, "
        "destination_remote_path TEXT NOT NULL, "
        "source_local_path TEXT NOT NULL, "
        "destination_local_path TEXT NOT NULL, "
        "local_device INTEGER NOT NULL, local_inode INTEGER NOT NULL, "
        "directory INTEGER NOT NULL, "
        "PRIMARY KEY (drive_id, remote_id));"
        "PRAGMA user_version = 21;"
    );
}

bool create_version_twenty_two_database(
    const std::filesystem::path& path
) {
    if (!create_version_twenty_one_database(path)) {
        return false;
    }
    return execute_schema(
        path,
        "ALTER TABLE item ADD COLUMN ctag TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE pending_download ADD COLUMN ctag "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE partial_download ADD COLUMN ctag "
        "TEXT NOT NULL DEFAULT '';"
        "PRAGMA user_version = 22;"
    );
}

bool create_version_twenty_three_database(
    const std::filesystem::path& path
) {
    if (!create_version_twenty_two_database(path)) {
        return false;
    }
    return execute_schema(
        path,
        "ALTER TABLE pending_upload ADD COLUMN failure_code "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE pending_upload ADD COLUMN failure_message "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE pending_upload ADD COLUMN failure_attempt_count "
        "INTEGER NOT NULL DEFAULT 0;"
        "PRAGMA user_version = 23;"
    );
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

bool schema_version_is(const std::filesystem::path& path, int expected) {
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const bool prepared = sqlite3_prepare_v2(
        database,
        "PRAGMA user_version;",
        -1,
        &statement,
        nullptr
    ) == SQLITE_OK;
    const bool valid =
        prepared && sqlite3_step(statement) == SQLITE_ROW &&
        sqlite3_column_int(statement, 0) == expected;
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return valid;
}

bool create_current_database(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    try {
        onedrive::storage::ItemDatabase database{directory, identity()};
        database.open();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<std::string> database_open_error(
    const std::filesystem::path& directory
) {
    try {
        onedrive::storage::ItemDatabase database{directory, identity()};
        database.open();
    } catch (const std::exception& error) {
        return error.what();
    }
    return std::nullopt;
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
        const auto drive_items = database.drive_items("me");
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
                std::optional<std::string>{"filter-1"} ||
            drive_items.size() != 1 ||
            drive_items[0].remote_id != "remote-3") {
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
                    .ctag = "blocked-ctag",
                    .remote_path = "blocked-me.txt",
                    .deleted = true,
                    .reason_code = "local_modification",
                    .reason_message = "local file was modified",
                    .content_hash = onedrive::util::FileHash{
                        .algorithm = onedrive::util::FileHashAlgorithm::quick_xor,
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
                .ctag = "pending-ctag",
                .remote_path = "pending-me.txt",
                .local_path = temporary_directory.path() / "pending-me.txt",
                .size = 4,
            },
            .temporary_path = temporary_directory.path() / "pending-me.tmp",
            .content_fingerprint = "fingerprint-me",
            .backup_path =
                temporary_directory.path() /
                "pending-me.safeBackup-20261004T051000Z-0001.txt",
            .backup_fingerprint = "backup-fingerprint-me",
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
                .ctag = "partial-ctag",
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
        database.save_pending_move({
            .drive_id = "me",
            .remote_id = "move-me",
            .source_path = temporary_directory.path() / "old-me.txt",
            .destination_path = temporary_directory.path() / "new-me.txt",
            .source_device = 10,
            .source_inode = 20,
        });
        database.save_pending_move({
            .drive_id = "other-drive",
            .remote_id = "move-other",
            .source_path = temporary_directory.path() / "old-other.txt",
            .destination_path = temporary_directory.path() / "new-other.txt",
            .source_device = 30,
            .source_inode = 40,
        });
        database.apply_delta({
            .drive_id = "me",
            .upload_suppressions = {
                {
                    .remote_id = "suppressed-me",
                    .local_path =
                        temporary_directory.path() / "suppressed-me.txt",
                    .source_device = 50,
                    .source_inode = 60,
                },
            },
            .delta_link = "https://graph.example.test/delta-me",
        });
        database.apply_delta({
            .drive_id = "other-drive",
            .upload_suppressions = {
                {
                    .remote_id = "suppressed-other",
                    .local_path =
                        temporary_directory.path() / "suppressed-other.txt",
                    .source_device = 70,
                    .source_inode = 80,
                },
            },
            .delta_link = "https://graph.example.test/delta-other",
        });
        const auto pending_me = database.pending_downloads("me");
        const auto partial_me =
            database.partial_download("me", "partial-me");
        if (pending_me.size() != 1 ||
            database.pending_downloads("other-drive").size() != 1 ||
            pending_me[0].item.ctag != "pending-ctag" ||
            pending_me[0].backup_path !=
                temporary_directory.path() /
                    "pending-me.safeBackup-20261004T051000Z-0001.txt" ||
            pending_me[0].backup_fingerprint != "backup-fingerprint-me" ||
            !partial_me || partial_me->item.ctag != "partial-ctag" ||
            !database.partial_download("other-drive", "partial-other") ||
            database.pending_moves("me").size() != 1 ||
            database.pending_moves("other-drive").size() != 1 ||
            database.upload_suppressions("me").size() != 1 ||
            database.upload_suppressions("other-drive").size() != 1 ||
            database.blocked_items("me").size() != 1 ||
            database.blocked_items("other-drive").size() != 1 ||
            database.blocked_items("me")[0].attempt_count != 1 ||
            database.blocked_items("me")[0].ctag != "blocked-ctag" ||
            !database.blocked_items("me")[0].deleted ||
            !database.blocked_items("me")[0].content_hash ||
            database.blocked_items("me")[0].content_hash->value !=
                "SgAAAAAAAAAAAAAAAQAAAAAAAAA=") {
            return fail("pending downloads or blocked items were not saved by drive");
        }
        try {
            database.save_pending_download({
                .item = {
                    .drive_id = "me",
                    .remote_id = "invalid-backup-journal",
                },
                .temporary_path =
                    temporary_directory.path() / "invalid.tmp",
                .content_fingerprint = "fingerprint",
                .backup_path =
                    temporary_directory.path() / "invalid.safeBackup",
            });
            return fail(
                "incomplete safeBackup journal metadata was accepted"
            );
        } catch (const std::invalid_argument&) {
        }

        if (!database.reset("me") || database.size() != 4 ||
            !database.find("me", "reset-me") ||
            !database.find("other-drive", "keep-me") ||
            database.delta_link("me").has_value() ||
            database.pending_downloads("me").size() != 1 ||
            database.pending_downloads("other-drive").size() != 1 ||
            !database.partial_download("me", "partial-me") ||
            !database.partial_download("other-drive", "partial-other") ||
            database.pending_moves("me").size() != 1 ||
            database.pending_moves("other-drive").size() != 1 ||
            database.upload_suppressions("me").size() != 1 ||
            database.upload_suppressions("other-drive").size() != 1 ||
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
            database.pending_moves("me").size() != 1 ||
            database.pending_moves("other-drive").size() != 1 ||
            database.upload_suppressions("me").size() != 1 ||
            database.upload_suppressions("other-drive").size() != 1 ||
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
            database.pending_moves("me").size() != 1 ||
            database.pending_moves("other-drive").size() != 1 ||
            database.upload_suppressions("me").size() != 1 ||
            database.upload_suppressions("other-drive").size() != 1 ||
            database.blocked_items("me").size() != 1 ||
            database.blocked_items("me")[0].remote_id != "fresh-blocked-me" ||
            database.blocked_items("other-drive").size() != 1) {
            return fail("initial delta did not replace only the selected drive");
        }

        const auto cleared = database.clear("me");
        if (cleared.items != 1 || cleared.pending_downloads != 1 ||
            cleared.partial_downloads != 1 ||
            cleared.pending_moves != 1 ||
            cleared.upload_suppressions != 1 ||
            cleared.blocked_items != 1 || !cleared.delta_link ||
            database.size() != 3 ||
            database.find("me", "fresh-me") ||
            !database.find("other-drive", "keep-me") ||
            !database.find("", "remote-1") ||
            !database.find("", "remote-2") ||
            !database.pending_downloads("me").empty() ||
            database.pending_downloads("other-drive").size() != 1 ||
            !database.pending_moves("me").empty() ||
            database.pending_moves("other-drive").size() != 1 ||
            !database.upload_suppressions("me").empty() ||
            database.upload_suppressions("other-drive").size() != 1 ||
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
        database.remove_upload_suppression(
            "other-drive",
            temporary_directory.path() / "suppressed-other.txt"
        );
        if (!database.upload_suppressions("other-drive").empty()) {
            return fail("upload suppression removal was not persisted");
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

    struct LegacyMigrationFixture {
        const char* name;
        bool (*create)(const std::filesystem::path&);
    };
    constexpr LegacyMigrationFixture legacy_migrations[] = {
        {"version-one", create_version_one_database},
        {"version-two", create_version_two_database},
        {"version-three", create_version_three_database},
    };
    for (const auto& fixture : legacy_migrations) {
        const auto directory = temporary_directory.path() / fixture.name;
        const auto database_path = directory / "items.sqlite3";
        std::filesystem::create_directories(directory);
        if (!fixture.create(database_path)) {
            return fail(
                std::string{fixture.name} +
                " migration fixture could not be created"
            );
        }
        {
            onedrive::storage::ItemDatabase database{directory, identity()};
            database.open();
        }
        if (!schema_version_is(database_path, 24) ||
            !identity_row_is_valid(database_path)) {
            return fail(
                std::string{fixture.name} +
                " database was not migrated to the current schema"
            );
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

    const auto version_five_directory =
        temporary_directory.path() / "version-five";
    std::filesystem::create_directories(version_five_directory);
    if (!create_version_five_database(
            version_five_directory / "items.sqlite3"
        )) {
        return fail("version five migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_five_directory,
            identity()
        };
        database.open();
        if (!identity_row_is_valid(
                version_five_directory / "items.sqlite3"
            )) {
            return fail(
                "version five database did not gain account identity state"
            );
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
                    .content_hash = onedrive::util::FileHash{
                        .algorithm = onedrive::util::FileHashAlgorithm::sha256,
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
                onedrive::util::FileHashAlgorithm::sha256 ||
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

    const auto version_ten_directory =
        temporary_directory.path() / "version-ten";
    std::filesystem::create_directories(version_ten_directory);
    if (!create_version_ten_database(
            version_ten_directory / "items.sqlite3"
        )) {
        return fail("version ten migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_ten_directory,
            identity()
        };
        database.open();
        database.save_pending_download({
            .item = {
                .drive_id = "me",
                .remote_id = "safe-backup-after-migration",
                .name = "report.txt",
                .etag = "etag",
                .remote_path = "report.txt",
                .local_path = version_ten_directory / "report.txt",
                .last_modified = "2026-10-04T05:10:00Z",
                .size = 4,
            },
            .temporary_path = version_ten_directory / ".report.partial",
            .content_fingerprint =
                "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602261a9a923d3bb23adc8b7",
            .backup_path =
                version_ten_directory /
                "report.safeBackup-20261004T051000Z-0001.txt",
            .backup_fingerprint =
                "ca3704aa0b06f5954c79ee837faa152d84c3fb2ceca2ba352a4a014fab6e5e2c",
        });
        const auto pending =
            database.pending_downloads("me");
        if (pending.size() != 1 ||
            pending[0].backup_path.filename() !=
                "report.safeBackup-20261004T051000Z-0001.txt" ||
            pending[0].backup_fingerprint !=
                "ca3704aa0b06f5954c79ee837faa152d84c3fb2ceca2ba352a4a014fab6e5e2c") {
            return fail(
                "version ten database did not gain safeBackup journal state"
            );
        }
    }

    const auto version_eleven_directory =
        temporary_directory.path() / "version-eleven";
    std::filesystem::create_directories(version_eleven_directory);
    if (!create_version_eleven_database(
            version_eleven_directory / "items.sqlite3"
        )) {
        return fail("version eleven migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_eleven_directory,
            identity()
        };
        database.open();
        database.apply_delta({
            .drive_id = "me",
            .blocked_upserts = {
                {
                    .remote_id = "deleted-after-migration",
                    .name = "deleted.txt",
                    .remote_path = "deleted.txt",
                    .deleted = true,
                    .reason_code = "local_modification",
                    .reason_message = "local file changed",
                },
            },
            .delta_link = "https://graph.example.test/delta-v12",
        });
        const auto blocked = database.blocked_items("me");
        if (blocked.size() != 1 || !blocked[0].deleted) {
            return fail(
                "version eleven database did not gain deletion retry state"
            );
        }
    }

    const auto version_twelve_directory =
        temporary_directory.path() / "version-twelve";
    std::filesystem::create_directories(version_twelve_directory);
    if (!create_version_twelve_database(
            version_twelve_directory / "items.sqlite3"
        )) {
        return fail("version twelve migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_twelve_directory,
            identity()
        };
        database.open();
        const onedrive::storage::PendingUpload pending{
            .drive_id = "me",
            .remote_path = "upload.txt",
            .local_path = version_twelve_directory / "upload.txt",
            .snapshot_path =
                version_twelve_directory / ".upload.onedrive-upload-1",
            .content_fingerprint =
                "239f59ed55e737c77147cf55ad0c1b030b6d7ee748a7426952f9b852d5a935e5",
            .local_size = 7,
            .local_modified_ticks = 123,
        };
        database.save_pending_upload(pending);
        const auto uploads = database.pending_uploads("me");
        if (uploads.size() != 1 ||
            uploads[0].remote_path != "upload.txt" ||
            uploads[0].local_size != 7) {
            return fail(
                "version twelve database did not gain pending upload state"
            );
        }
        database.commit_upload(pending, {
            .drive_id = "me",
            .remote_id = "uploaded-id",
            .name = "upload.txt",
            .etag = "uploaded-etag",
            .remote_path = "upload.txt",
            .local_path = pending.local_path,
            .last_modified = "2026-10-04T09:00:00Z",
            .size = 7,
            .local_size = 7,
            .local_modified_ticks = 123,
        });
        if (!database.pending_uploads("me").empty() ||
            !database.find("me", "uploaded-id")) {
            return fail("pending upload commit was not atomic");
        }
    }

    const auto version_fifteen_directory =
        temporary_directory.path() / "version-fifteen";
    std::filesystem::create_directories(version_fifteen_directory);
    if (!create_version_fifteen_database(
            version_fifteen_directory / "items.sqlite3"
        )) {
        return fail("version fifteen migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_fifteen_directory,
            identity()
        };
        database.open();
        database.apply_delta({
            .drive_id = "me",
            .upload_suppressions = {
                {
                    .remote_id = "migrated-suppression",
                    .local_path =
                        version_fifteen_directory / "retained.txt",
                    .source_device = 111,
                    .source_inode = 222,
                },
            },
            .delta_link = "https://graph.example.test/v16",
        });
        const auto suppressions = database.upload_suppressions("me");
        if (suppressions.size() != 1 ||
            suppressions[0].remote_id != "migrated-suppression" ||
            suppressions[0].source_device != 111 ||
            suppressions[0].source_inode != 222) {
            return fail(
                "version fifteen database did not gain upload suppressions"
            );
        }
    }

    const auto version_sixteen_directory =
        temporary_directory.path() / "version-sixteen";
    std::filesystem::create_directories(version_sixteen_directory);
    if (!create_version_sixteen_database(
            version_sixteen_directory / "items.sqlite3"
        )) {
        return fail("version sixteen migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_sixteen_directory,
            identity()
        };
        database.open();
        const onedrive::storage::PendingUpload pending{
            .drive_id = "me",
            .remote_path = "large.bin",
            .local_path = version_sixteen_directory / "large.bin",
            .snapshot_path =
                version_sixteen_directory / ".large.onedrive-upload-1",
            .content_fingerprint =
                "239f59ed55e737c77147cf55ad0c1b030b6d7ee748a7426952f9b852d5a935e5",
            .local_size = 655360,
            .local_modified_ticks = 456,
            .upload_url = "https://upload.example.test/session?secret=1",
            .upload_expiration = "2099-10-05T09:00:00Z",
            .completed_bytes = 327680,
        };
        database.save_pending_upload(pending);
        const auto uploads = database.pending_uploads("me");
        if (uploads.size() != 1 ||
            uploads[0].upload_url != pending.upload_url ||
            uploads[0].upload_expiration != pending.upload_expiration ||
            uploads[0].completed_bytes != pending.completed_bytes) {
            return fail(
                "version sixteen database did not persist upload checkpoints"
            );
        }
    }

    const auto version_seventeen_directory =
        temporary_directory.path() / "version-seventeen";
    std::filesystem::create_directories(version_seventeen_directory);
    if (!create_version_seventeen_database(
            version_seventeen_directory / "items.sqlite3"
        )) {
        return fail(
            "version seventeen migration fixture could not be created"
        );
    }
    {
        onedrive::storage::ItemDatabase database{
            version_seventeen_directory,
            identity()
        };
        database.open();
        const onedrive::storage::PendingUpload pending{
            .drive_id = "me",
            .remote_path = "Parent/Child",
            .local_path =
                version_seventeen_directory / "Parent" / "Child",
            .snapshot_path = {},
            .content_fingerprint = {},
            .local_size = 0,
            .local_modified_ticks = 0,
            .remote_id = std::nullopt,
            .expected_etag = {},
            .upload_url = {},
            .upload_expiration = {},
            .completed_bytes = 0,
            .directory = true,
        };
        database.save_pending_upload(pending);
        const auto uploads = database.pending_uploads("me");
        if (uploads.size() != 1 ||
            !uploads[0].directory ||
            uploads[0].remote_path != pending.remote_path) {
            return fail(
                "version seventeen database did not persist directory uploads"
            );
        }
        database.remove_pending_upload("me", pending.remote_path);
        if (!database.pending_uploads("me").empty()) {
            return fail("pending directory upload was not removed");
        }
    }

    const auto version_eighteen_directory =
        temporary_directory.path() / "version-eighteen";
    std::filesystem::create_directories(version_eighteen_directory);
    if (!create_version_eighteen_database(
            version_eighteen_directory / "items.sqlite3"
        )) {
        return fail(
            "version eighteen migration fixture could not be created"
        );
    }
    {
        onedrive::storage::ItemDatabase database{
            version_eighteen_directory,
            identity()
        };
        database.open();
        database.upsert({
            .drive_id = "me",
            .remote_id = "deleted-directory",
            .name = "Deleted",
            .etag = "delete-etag",
            .remote_path = "Deleted",
            .local_path = version_eighteen_directory / "Deleted",
            .directory = true,
        });
        database.upsert({
            .drive_id = "me",
            .remote_id = "deleted-child",
            .parent_id = "deleted-directory",
            .name = "child.txt",
            .etag = "child-etag",
            .remote_path = "Deleted/child.txt",
            .local_path =
                version_eighteen_directory / "Deleted" / "child.txt",
        });
        const onedrive::storage::PendingDelete deletion{
            .drive_id = "me",
            .remote_id = "deleted-directory",
            .expected_etag = "delete-etag",
            .remote_path = "Deleted",
            .local_path = version_eighteen_directory / "Deleted",
            .directory = true,
        };
        database.save_pending_delete(deletion);
        const auto deletions = database.pending_deletes("me");
        if (deletions.size() != 1 ||
            deletions[0].expected_etag != "delete-etag") {
            return fail(
                "version eighteen database did not gain deletion journal"
            );
        }
        database.commit_delete(deletion);
        if (!database.pending_deletes("me").empty() ||
            database.find("me", "deleted-directory") ||
            database.find("me", "deleted-child")) {
            return fail("pending directory deletion commit was not atomic");
        }
    }

    const auto version_nineteen_directory =
        temporary_directory.path() / "version-nineteen";
    std::filesystem::create_directories(version_nineteen_directory);
    if (!create_version_nineteen_database(
            version_nineteen_directory / "items.sqlite3"
        )) {
        return fail(
            "version nineteen migration fixture could not be created"
        );
    }
    {
        onedrive::storage::ItemDatabase database{
            version_nineteen_directory,
            identity()
        };
        database.open();
        database.upsert({
            .drive_id = "me",
            .remote_id = "identity-item",
            .name = "identity.txt",
            .etag = "identity-etag",
            .remote_path = "identity.txt",
            .local_path = version_nineteen_directory / "identity.txt",
            .local_device = 123,
            .local_inode = 456,
        });
        const auto item = database.find("me", "identity-item");
        if (!item || item->local_device != 123 ||
            item->local_inode != 456) {
            return fail(
                "version nineteen database did not persist local identity"
            );
        }
    }

    const auto version_twenty_directory =
        temporary_directory.path() / "version-twenty";
    std::filesystem::create_directories(version_twenty_directory);
    if (!create_version_twenty_database(
            version_twenty_directory / "items.sqlite3"
        )) {
        return fail("version twenty migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_twenty_directory,
            identity()
        };
        database.open();
        database.upsert({
            .drive_id = "me",
            .remote_id = "move-directory",
            .name = "Old",
            .etag = "old-etag",
            .remote_path = "Old",
            .local_path = version_twenty_directory / "Old",
            .local_device = 11,
            .local_inode = 22,
            .directory = true,
        });
        database.upsert({
            .drive_id = "me",
            .remote_id = "move-child",
            .parent_id = "move-directory",
            .name = "child.txt",
            .etag = "child-etag",
            .remote_path = "Old/child.txt",
            .local_path = version_twenty_directory / "Old" / "child.txt",
            .local_device = 11,
            .local_inode = 23,
        });
        const onedrive::storage::PendingRemoteMove move{
            .drive_id = "me",
            .remote_id = "move-directory",
            .expected_etag = "old-etag",
            .source_remote_path = "Old",
            .destination_remote_path = "New",
            .source_local_path = version_twenty_directory / "Old",
            .destination_local_path = version_twenty_directory / "New",
            .local_device = 11,
            .local_inode = 22,
            .directory = true,
        };
        database.save_pending_remote_move(move);
        if (database.pending_remote_moves("me").size() != 1) {
            return fail(
                "version twenty database did not gain remote move journal"
            );
        }
        database.commit_remote_move(move, {
            .drive_id = "me",
            .remote_id = "move-directory",
            .name = "New",
            .etag = "new-etag",
            .remote_path = "New",
            .local_path = version_twenty_directory / "New",
            .local_device = 11,
            .local_inode = 22,
            .directory = true,
        });
        const auto child = database.find("me", "move-child");
        if (!database.pending_remote_moves("me").empty() || !child ||
            child->remote_path != "New/child.txt" ||
            child->local_path !=
                version_twenty_directory / "New" / "child.txt") {
            return fail("remote directory move commit was not atomic");
        }
    }

    const auto version_twenty_one_directory =
        temporary_directory.path() / "version-twenty-one";
    std::filesystem::create_directories(version_twenty_one_directory);
    if (!create_version_twenty_one_database(
            version_twenty_one_directory / "items.sqlite3"
        )) {
        return fail(
            "version twenty-one migration fixture could not be created"
        );
    }
    {
        onedrive::storage::ItemDatabase database{
            version_twenty_one_directory,
            identity()
        };
        database.open();
        database.upsert({
            .drive_id = "me",
            .remote_id = "ctag-item",
            .name = "ctag.txt",
            .etag = "metadata-version",
            .ctag = "content-version",
            .remote_path = "ctag.txt",
            .local_path = version_twenty_one_directory / "ctag.txt",
        });
        const auto item = database.find("me", "ctag-item");
        if (!item || item->ctag != "content-version") {
            return fail(
                "version twenty-one database did not persist content tags"
            );
        }
    }

    const auto version_twenty_two_directory =
        temporary_directory.path() / "version-twenty-two";
    std::filesystem::create_directories(version_twenty_two_directory);
    if (!create_version_twenty_two_database(
            version_twenty_two_directory / "items.sqlite3"
        )) {
        return fail(
            "version twenty-two migration fixture could not be created"
        );
    }
    {
        onedrive::storage::ItemDatabase database{
            version_twenty_two_directory,
            identity()
        };
        database.open();
        const onedrive::storage::PendingUpload failure{
            .drive_id = "me",
            .remote_path = "quota.txt",
            .local_path = version_twenty_two_directory / "quota.txt",
            .snapshot_path =
                version_twenty_two_directory / ".quota.upload",
            .content_fingerprint = "fingerprint",
            .local_size = 5,
            .failure_code = "remote_quota",
            .failure_message = "OneDrive quota exceeded",
            .failure_attempt_count = 2,
        };
        database.save_pending_upload(failure);
        const auto uploads = database.pending_uploads("me");
        if (uploads.size() != 1 ||
            uploads[0].failure_code != failure.failure_code ||
            uploads[0].failure_message != failure.failure_message ||
            uploads[0].failure_attempt_count != 2) {
            return fail(
                "version twenty-two database did not persist upload failures"
            );
        }
    }

    const auto version_twenty_three_directory =
        temporary_directory.path() / "version-twenty-three";
    const auto version_twenty_three_path =
        version_twenty_three_directory / "items.sqlite3";
    std::filesystem::create_directories(version_twenty_three_directory);
    if (!create_version_twenty_three_database(version_twenty_three_path) ||
        !execute_schema(
            version_twenty_three_path,
            "INSERT INTO blocked_item ("
            "drive_id, remote_id, parent_id, name, etag, remote_path, "
            "last_modified, size, directory, deleted, reason_code, "
            "reason_message"
            ") VALUES ("
            "'me', 'blocked-ctag', 'root', 'blocked.txt', 'etag', "
            "'blocked.txt', '2026-10-05T00:00:00Z', 4, 0, 0, "
            "'local_modification', 'local file changed'"
            ");"
        )) {
        return fail(
            "version twenty-three migration fixture could not be created"
        );
    }
    {
        onedrive::storage::ItemDatabase database{
            version_twenty_three_directory,
            identity()
        };
        database.open();
        const auto migrated = database.blocked_items("me");
        if (migrated.size() != 1 || !migrated[0].ctag.empty()) {
            return fail(
                "version twenty-three blocked item ctag was not migrated"
            );
        }
        database.apply_delta({
            .drive_id = "me",
            .blocked_upserts = {
                {
                    .remote_id = "blocked-ctag",
                    .name = "blocked.txt",
                    .etag = "metadata-version",
                    .ctag = "content-version",
                    .remote_path = "blocked.txt",
                    .reason_code = "local_modification",
                    .reason_message = "local file changed",
                },
            },
            .delta_link = "https://graph.example.test/blocked-ctag",
        });
        const auto updated = database.blocked_items("me");
        if (updated.size() != 1 ||
            updated[0].ctag != "content-version") {
            return fail("blocked item content tag was not persisted");
        }
    }
    if (!schema_version_is(version_twenty_three_path, 24)) {
        return fail(
            "version twenty-three database was not migrated to version 24"
        );
    }

    const auto version_fourteen_directory =
        temporary_directory.path() / "version-fourteen";
    std::filesystem::create_directories(version_fourteen_directory);
    if (!create_version_fourteen_database(
            version_fourteen_directory / "items.sqlite3"
        )) {
        return fail("version fourteen migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_fourteen_directory,
            identity()
        };
        database.open();
        auto moves = database.pending_moves("me");
        if (moves.size() != 1 ||
            moves[0].remote_id != "legacy-move" ||
            !moves[0].staging_path.empty()) {
            return fail(
                "version fourteen pending move did not migrate to staging"
            );
        }
        const onedrive::storage::PendingMove staged{
            .drive_id = "me",
            .remote_id = "staged-move",
            .source_path = version_fourteen_directory / "A.txt",
            .destination_path = version_fourteen_directory / "B.txt",
            .staging_path =
                version_fourteen_directory /
                ".A.txt.onedrive-move-test",
            .source_device = 789,
            .source_inode = 987,
        };
        database.save_pending_move(staged);
        moves = database.pending_moves("me");
        const auto saved = std::ranges::find(
            moves,
            "staged-move",
            &onedrive::storage::PendingMove::remote_id
        );
        if (saved == moves.end() ||
            saved->staging_path != staged.staging_path ||
            saved->source_device != staged.source_device ||
            saved->source_inode != staged.source_inode) {
            return fail("staged pending move did not round trip");
        }
    }

    const auto version_thirteen_directory =
        temporary_directory.path() / "version-thirteen";
    std::filesystem::create_directories(version_thirteen_directory);
    if (!create_version_thirteen_database(
            version_thirteen_directory / "items.sqlite3"
        )) {
        return fail("version thirteen migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_thirteen_directory,
            identity()
        };
        database.open();
        const onedrive::storage::PendingMove pending{
            .drive_id = "me",
            .remote_id = "moved-id",
            .source_path = version_thirteen_directory / "old.txt",
            .destination_path = version_thirteen_directory / "new.txt",
            .source_device = 123,
            .source_inode = 456,
        };
        database.save_pending_move(pending);
        const auto moves = database.pending_moves("me");
        if (moves.size() != 1 ||
            moves[0].source_path != pending.source_path ||
            moves[0].destination_path != pending.destination_path ||
            moves[0].source_device != 123 ||
            moves[0].source_inode != 456) {
            return fail(
                "version thirteen database did not gain pending move state"
            );
        }
        try {
            database.apply_delta({
                .drive_id = "me",
                .upserts = {
                    {
                        .remote_id = "moved-id",
                        .name = "new.txt",
                        .remote_path = "new.txt",
                        .local_path = pending.destination_path,
                    },
                },
                .blocked_upserts = {
                    {
                        .remote_id = "invalid-blocked",
                    },
                },
                .delta_link = "https://graph.example.test/rollback",
            });
            return fail("invalid delta did not roll back pending move commit");
        } catch (const std::invalid_argument&) {
        }
        if (database.pending_moves("me").size() != 1 ||
            database.find("me", "moved-id")) {
            return fail("pending move was not restored by delta rollback");
        }
        database.apply_delta({
            .drive_id = "me",
            .upserts = {
                {
                    .remote_id = "moved-id",
                    .name = "new.txt",
                    .remote_path = "new.txt",
                    .local_path = pending.destination_path,
                },
            },
            .delta_link = "https://graph.example.test/moved",
        });
        if (!database.pending_moves("me").empty() ||
            !database.find("me", "moved-id")) {
            return fail("pending move commit was not atomic");
        }
    }

    const auto assert_rejected =
        [](const std::filesystem::path& directory,
           std::string_view expected,
           std::string_view description) {
        const auto error = database_open_error(directory);
        if (!error || !error->contains(expected) ||
            !error->contains("Move or remove this database file")) {
            return fail(
                std::string{description} +
                " was not rejected with recovery guidance"
            );
        }
        return EXIT_SUCCESS;
    };

    const auto missing_table_directory =
        temporary_directory.path() / "schema-missing-table";
    if (!create_current_database(missing_table_directory) ||
        !execute_schema(
            missing_table_directory / "items.sqlite3",
            "DROP TABLE pending_delete;"
        ) ||
        assert_rejected(
            missing_table_directory,
            "missing table 'pending_delete'",
            "database with a missing table"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto missing_column_directory =
        temporary_directory.path() / "schema-missing-column";
    if (!create_current_database(missing_column_directory) ||
        !execute_schema(
            missing_column_directory / "items.sqlite3",
            "ALTER TABLE item DROP COLUMN ctag;"
        ) ||
        assert_rejected(
            missing_column_directory,
            "missing column 'ctag'",
            "database with a missing column"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto extra_column_directory =
        temporary_directory.path() / "schema-extra-column";
    if (!create_current_database(extra_column_directory) ||
        !execute_schema(
            extra_column_directory / "items.sqlite3",
            "ALTER TABLE drive_state ADD COLUMN unexpected TEXT;"
        ) ||
        assert_rejected(
            extra_column_directory,
            "unexpected column 'unexpected'",
            "database with an extra column"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto extra_table_directory =
        temporary_directory.path() / "schema-extra-table";
    if (!create_current_database(extra_table_directory) ||
        !execute_schema(
            extra_table_directory / "items.sqlite3",
            "CREATE TABLE unexpected (value TEXT);"
        ) ||
        assert_rejected(
            extra_table_directory,
            "unexpected table 'unexpected'",
            "database with an extra table"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto extra_index_directory =
        temporary_directory.path() / "schema-extra-index";
    if (!create_current_database(extra_index_directory) ||
        !execute_schema(
            extra_index_directory / "items.sqlite3",
            "CREATE INDEX unexpected_item_name ON item(name);"
        ) ||
        assert_rejected(
            extra_index_directory,
            "incompatible primary key or index definitions",
            "database with an extra index"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto wrong_primary_key_directory =
        temporary_directory.path() / "schema-wrong-primary-key";
    if (!create_current_database(wrong_primary_key_directory) ||
        !execute_schema(
            wrong_primary_key_directory / "items.sqlite3",
            "DROP TABLE upload_suppression;"
            "CREATE TABLE upload_suppression ("
            "drive_id TEXT NOT NULL, remote_id TEXT NOT NULL, "
            "local_path TEXT NOT NULL, source_device INTEGER NOT NULL, "
            "source_inode INTEGER NOT NULL, "
            "PRIMARY KEY (drive_id, remote_id));"
        ) ||
        assert_rejected(
            wrong_primary_key_directory,
            "incompatible definition",
            "database with an incorrect primary key"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto future_version_directory =
        temporary_directory.path() / "schema-future-version";
    if (!create_current_database(future_version_directory) ||
        !execute_schema(
            future_version_directory / "items.sqlite3",
            "PRAGMA user_version = 25;"
        ) ||
        assert_rejected(
            future_version_directory,
            "unsupported state database schema version 25",
            "database with a future schema version"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto unversioned_directory =
        temporary_directory.path() / "schema-unversioned";
    std::filesystem::create_directories(unversioned_directory);
    if (!execute_schema(
            unversioned_directory / "items.sqlite3",
            "CREATE TABLE unexpected (value TEXT);"
        ) ||
        assert_rejected(
            unversioned_directory,
            "unversioned state database contains existing tables",
            "unversioned non-empty database"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto interrupted_directory =
        temporary_directory.path() / "schema-interrupted-migration";
    const auto interrupted_path = interrupted_directory / "items.sqlite3";
    std::filesystem::create_directories(interrupted_directory);
    if (!create_version_twenty_two_database(interrupted_path) ||
        !execute_schema(
            interrupted_path,
            "ALTER TABLE pending_upload ADD COLUMN failure_code "
            "TEXT NOT NULL DEFAULT '';"
        ) ||
        assert_rejected(
            interrupted_directory,
            "duplicate column name: failure_code",
            "partially applied migration"
        ) != EXIT_SUCCESS ||
        !schema_version_is(interrupted_path, 22)) {
        return fail(
            "partially applied migration changed the saved schema version"
        );
    }

    const auto corrupt_directory =
        temporary_directory.path() / "schema-corrupt";
    std::filesystem::create_directories(corrupt_directory);
    {
        std::ofstream corrupt{
            corrupt_directory / "items.sqlite3",
            std::ios::binary
        };
        corrupt << "not a SQLite database";
    }
    if (assert_rejected(
            corrupt_directory,
            "file is not a database",
            "corrupt database"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
