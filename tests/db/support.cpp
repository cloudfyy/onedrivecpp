#include "onedrive/storage/item_database.hpp"
#include "support.hpp"
#include "test_support.hpp"

#include <sqlite3.h>

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace onedrive::test::db {

using onedrive::test::fail;
using onedrive::test::TemporaryDirectory;

struct SqliteCloser {
    void operator()(sqlite3* database) const noexcept {
        sqlite3_close(database);
    }
};

struct SqliteStatementFinalizer {
    void operator()(sqlite3_stmt* statement) const noexcept {
        sqlite3_finalize(statement);
    }
};

using TestDatabase = std::unique_ptr<sqlite3, SqliteCloser>;
using TestStatement = std::unique_ptr<sqlite3_stmt, SqliteStatementFinalizer>;

TestDatabase open_database(const std::filesystem::path& path) {
    sqlite3* raw_database = nullptr;
    const auto result = sqlite3_open(path.string().c_str(), &raw_database);
    TestDatabase database{raw_database};
    if (result != SQLITE_OK) {
        return {};
    }
    return database;
}

TestStatement prepare_statement(sqlite3* database, const char* sql) {
    sqlite3_stmt* raw_statement = nullptr;
    const auto result =
        sqlite3_prepare_v2(database, sql, -1, &raw_statement, nullptr);
    TestStatement statement{raw_statement};
    if (result != SQLITE_OK) {
        return {};
    }
    return statement;
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

bool execute_schema(const std::filesystem::path& path, const char* schema) {
    auto database = open_database(path);
    if (!database) {
        return false;
    }
    const bool succeeded =
        sqlite3_exec(database.get(), schema, nullptr, nullptr, nullptr) ==
        SQLITE_OK;
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
    auto database = open_database(path);
    if (!database) {
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
        sqlite3_exec(database.get(), schema, nullptr, nullptr, nullptr) ==
        SQLITE_OK;
    return succeeded;
}

bool create_version_five_database(const std::filesystem::path& path) {
    if (!create_version_four_database(path)) {
        return false;
    }
    auto database = open_database(path);
    if (!database) {
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
        sqlite3_exec(database.get(), schema, nullptr, nullptr, nullptr) ==
        SQLITE_OK;
    return succeeded;
}

bool create_version_six_database(const std::filesystem::path& path) {
    if (!create_version_four_database(path)) {
        return false;
    }
    auto database = open_database(path);
    if (!database) {
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
        sqlite3_exec(database.get(), schema, nullptr, nullptr, nullptr) ==
        SQLITE_OK;
    return succeeded;
}

bool create_version_seven_database(const std::filesystem::path& path) {
    if (!create_version_six_database(path)) {
        return false;
    }
    auto database = open_database(path);
    if (!database) {
        return false;
    }
    constexpr const char* schema =
        "CREATE TABLE drive_mapping ("
        "configured_drive_id TEXT PRIMARY KEY NOT NULL, "
        "canonical_drive_id TEXT NOT NULL, "
        "last_resolved INTEGER NOT NULL DEFAULT (unixepoch()));"
        "PRAGMA user_version = 7;";
    const bool succeeded =
        sqlite3_exec(database.get(), schema, nullptr, nullptr, nullptr) ==
        SQLITE_OK;
    return succeeded;
}

bool create_version_eight_database(const std::filesystem::path& path) {
    if (!create_version_seven_database(path)) {
        return false;
    }

    auto database = open_database(path);
    if (!database) {
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
        sqlite3_exec(database.get(), schema, nullptr, nullptr, nullptr) ==
        SQLITE_OK;
    return succeeded;
}

bool create_version_nine_database(const std::filesystem::path& path) {
    if (!create_version_eight_database(path)) {
        return false;
    }
    auto database = open_database(path);
    if (!database) {
        return false;
    }
    constexpr const char* schema =
        "ALTER TABLE blocked_item ADD COLUMN content_hash_algorithm "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE blocked_item ADD COLUMN content_hash_value "
        "TEXT NOT NULL DEFAULT '';"
        "PRAGMA user_version = 9;";
    const bool succeeded =
        sqlite3_exec(database.get(), schema, nullptr, nullptr, nullptr) ==
        SQLITE_OK;
    return succeeded;
}

bool create_version_ten_database(const std::filesystem::path& path) {
    if (!create_version_nine_database(path)) {
        return false;
    }
    auto database = open_database(path);
    if (!database) {
        return false;
    }
    constexpr const char* schema =
        "ALTER TABLE drive_state ADD COLUMN sync_filter_fingerprint "
        "TEXT NOT NULL DEFAULT '';"
        "PRAGMA user_version = 10;";
    const bool succeeded =
        sqlite3_exec(database.get(), schema, nullptr, nullptr, nullptr) ==
        SQLITE_OK;
    return succeeded;
}

bool create_version_eleven_database(const std::filesystem::path& path) {
    if (!create_version_ten_database(path)) {
        return false;
    }
    auto database = open_database(path);
    if (!database) {
        return false;
    }
    constexpr const char* schema =
        "ALTER TABLE pending_download ADD COLUMN backup_path "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE pending_download ADD COLUMN backup_fingerprint "
        "TEXT NOT NULL DEFAULT '';"
        "PRAGMA user_version = 11;";
    const bool succeeded =
        sqlite3_exec(database.get(), schema, nullptr, nullptr, nullptr) ==
        SQLITE_OK;
    return succeeded;
}

bool create_version_twelve_database(const std::filesystem::path& path) {
    if (!create_version_eleven_database(path)) {
        return false;
    }
    auto database = open_database(path);
    if (!database) {
        return false;
    }
    constexpr const char* schema =
        "ALTER TABLE blocked_item ADD COLUMN deleted "
        "INTEGER NOT NULL DEFAULT 0;"
        "PRAGMA user_version = 12;";
    const bool succeeded =
        sqlite3_exec(database.get(), schema, nullptr, nullptr, nullptr) ==
        SQLITE_OK;
    return succeeded;
}

bool create_version_thirteen_database(const std::filesystem::path& path) {
    if (!create_version_twelve_database(path)) {
        return false;
    }
    auto database = open_database(path);
    if (!database) {
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
        sqlite3_exec(database.get(), schema, nullptr, nullptr, nullptr) ==
        SQLITE_OK;
    return succeeded;
}

bool create_version_fourteen_database(const std::filesystem::path& path) {
    if (!create_version_thirteen_database(path)) {
        return false;
    }
    auto database = open_database(path);
    if (!database) {
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
        sqlite3_exec(database.get(), schema, nullptr, nullptr, nullptr) ==
        SQLITE_OK;
    return succeeded;
}

bool create_version_fifteen_database(const std::filesystem::path& path) {
    if (!create_version_fourteen_database(path)) {
        return false;
    }
    auto database = open_database(path);
    if (!database) {
        return false;
    }
    constexpr const char* migration =
        "ALTER TABLE pending_move ADD COLUMN staging_path "
        "TEXT NOT NULL DEFAULT '';"
        "PRAGMA user_version = 15;";
    const bool succeeded =
        sqlite3_exec(database.get(), migration, nullptr, nullptr, nullptr) ==
        SQLITE_OK;
    return succeeded;
}

bool create_version_sixteen_database(const std::filesystem::path& path) {
    if (!create_version_fifteen_database(path)) {
        return false;
    }
    auto database = open_database(path);
    if (!database) {
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
        sqlite3_exec(database.get(), migration, nullptr, nullptr, nullptr) ==
        SQLITE_OK;
    return succeeded;
}

bool create_version_seventeen_database(const std::filesystem::path& path) {
    if (!create_version_sixteen_database(path)) {
        return false;
    }
    auto database = open_database(path);
    if (!database) {
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
        sqlite3_exec(database.get(), migration, nullptr, nullptr, nullptr) ==
        SQLITE_OK;
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

bool create_version_twenty_one_database(const std::filesystem::path& path) {
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

bool create_version_twenty_two_database(const std::filesystem::path& path) {
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

bool create_version_twenty_three_database(const std::filesystem::path& path) {
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
    auto database = open_database(path);
    if (!database) {
        return false;
    }
    auto identity_statement = prepare_statement(
        database.get(),
        "SELECT user_id, user_display_name, drive_id, drive_name, "
        "avatar_content_type, length(avatar_bytes) FROM identity "
        "WHERE singleton = 1;"
    );
    const bool valid = identity_statement &&
                       sqlite3_step(identity_statement.get()) == SQLITE_ROW &&
                       std::string{reinterpret_cast<const char*>(
                           sqlite3_column_text(identity_statement.get(), 0)
                       )} == "user-id" &&
                       std::string{reinterpret_cast<const char*>(
                           sqlite3_column_text(identity_statement.get(), 1)
                       )} == "Test User" &&
                       std::string{reinterpret_cast<const char*>(
                           sqlite3_column_text(identity_statement.get(), 2)
                       )} == "canonical-drive-id" &&
                       std::string{reinterpret_cast<const char*>(
                           sqlite3_column_text(identity_statement.get(), 3)
                       )} == "Test Drive" &&
                       std::string{reinterpret_cast<const char*>(
                           sqlite3_column_text(identity_statement.get(), 4)
                       )} == "image/jpeg" &&
                       sqlite3_column_int(identity_statement.get(), 5) == 4;
    auto mapping_statement = prepare_statement(
        database.get(),
        "SELECT canonical_drive_id FROM drive_mapping "
        "WHERE configured_drive_id = 'me';"
    );
    const bool mapping_valid =
        mapping_statement &&
        sqlite3_step(mapping_statement.get()) == SQLITE_ROW &&
        std::string{reinterpret_cast<const char*>(
            sqlite3_column_text(mapping_statement.get(), 0)
        )} == "canonical-drive-id";
    return valid && mapping_valid;
}

bool schema_version_is(const std::filesystem::path& path, int expected) {
    auto database = open_database(path);
    if (!database) {
        return false;
    }
    auto statement = prepare_statement(database.get(), "PRAGMA user_version;");
    const bool valid = statement &&
                       sqlite3_step(statement.get()) == SQLITE_ROW &&
                       sqlite3_column_int(statement.get(), 0) == expected;
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

std::optional<std::string>
database_open_error(const std::filesystem::path& directory) {
    try {
        onedrive::storage::ItemDatabase database{directory, identity()};
        database.open();
    } catch (const std::exception& error) {
        return error.what();
    }
    return std::nullopt;
}

} // namespace onedrive::test::db
