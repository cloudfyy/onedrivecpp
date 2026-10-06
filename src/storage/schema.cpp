#include "storage/schema.hpp"

#include "storage/sqlite_support.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace onedrive::storage::item_database_detail {

constexpr int maximum_database_pages = 8 * 1024 * 1024;

int schema_version(sqlite3* database) {
    Statement statement{database, "PRAGMA user_version;"};
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw std::runtime_error(
            "cannot read state database schema version: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    return sqlite3_column_int(statement.get(), 0);
}

void create_item_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE IF NOT EXISTS item ("
        "drive_id TEXT NOT NULL,"
        "remote_id TEXT NOT NULL,"
        "parent_id TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "etag TEXT NOT NULL,"
        "ctag TEXT NOT NULL DEFAULT '',"
        "remote_path TEXT NOT NULL,"
        "local_path TEXT NOT NULL,"
        "last_modified TEXT NOT NULL,"
        "size INTEGER NOT NULL,"
        "local_size INTEGER NOT NULL,"
        "local_modified_ticks INTEGER NOT NULL,"
        "local_device INTEGER NOT NULL DEFAULT 0,"
        "local_inode INTEGER NOT NULL DEFAULT 0,"
        "directory INTEGER NOT NULL,"
        "PRIMARY KEY (drive_id, remote_id)"
        ");"
    );
}

void create_drive_state_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE IF NOT EXISTS drive_state ("
        "drive_id TEXT PRIMARY KEY NOT NULL,"
        "delta_link TEXT NOT NULL,"
        "sync_filter_fingerprint TEXT NOT NULL DEFAULT ''"
        ");"
    );
}

void create_pending_download_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE IF NOT EXISTS pending_download ("
        "drive_id TEXT NOT NULL,"
        "remote_id TEXT NOT NULL,"
        "parent_id TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "etag TEXT NOT NULL,"
        "ctag TEXT NOT NULL DEFAULT '',"
        "remote_path TEXT NOT NULL,"
        "local_path TEXT NOT NULL,"
        "last_modified TEXT NOT NULL,"
        "size INTEGER NOT NULL,"
        "directory INTEGER NOT NULL,"
        "temporary_path TEXT NOT NULL,"
        "content_fingerprint TEXT NOT NULL,"
        "backup_path TEXT NOT NULL DEFAULT '',"
        "backup_fingerprint TEXT NOT NULL DEFAULT '',"
        "PRIMARY KEY (drive_id, remote_id)"
        ");"
    );
}

void create_blocked_item_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE IF NOT EXISTS blocked_item ("
        "drive_id TEXT NOT NULL,"
        "remote_id TEXT NOT NULL,"
        "parent_id TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "etag TEXT NOT NULL,"
        "ctag TEXT NOT NULL DEFAULT '',"
        "remote_path TEXT NOT NULL,"
        "last_modified TEXT NOT NULL,"
        "size INTEGER NOT NULL,"
        "directory INTEGER NOT NULL,"
        "deleted INTEGER NOT NULL DEFAULT 0,"
        "reason_code TEXT NOT NULL,"
        "reason_message TEXT NOT NULL,"
        "content_hash_algorithm TEXT NOT NULL DEFAULT '',"
        "content_hash_value TEXT NOT NULL DEFAULT '',"
        "first_seen INTEGER NOT NULL DEFAULT (unixepoch()),"
        "last_attempt INTEGER NOT NULL DEFAULT (unixepoch()),"
        "attempt_count INTEGER NOT NULL DEFAULT 1,"
        "PRIMARY KEY (drive_id, remote_id)"
        ");"
    );
}

void create_blocked_item_v5_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE blocked_item ("
        "drive_id TEXT NOT NULL,"
        "remote_id TEXT NOT NULL,"
        "parent_id TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "etag TEXT NOT NULL,"
        "remote_path TEXT NOT NULL,"
        "last_modified TEXT NOT NULL,"
        "size INTEGER NOT NULL,"
        "directory INTEGER NOT NULL,"
        "reason_code TEXT NOT NULL,"
        "reason_message TEXT NOT NULL,"
        "first_seen INTEGER NOT NULL DEFAULT (unixepoch()),"
        "last_attempt INTEGER NOT NULL DEFAULT (unixepoch()),"
        "attempt_count INTEGER NOT NULL DEFAULT 1,"
        "PRIMARY KEY (drive_id, remote_id)"
        ");"
    );
}

void add_blocked_item_hash_columns(sqlite3* database) {
    execute(
        database,
        "ALTER TABLE blocked_item ADD COLUMN content_hash_algorithm "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE blocked_item ADD COLUMN content_hash_value "
        "TEXT NOT NULL DEFAULT '';"
    );
}

void add_blocked_item_deleted_column(sqlite3* database) {
    execute(
        database,
        "ALTER TABLE blocked_item ADD COLUMN deleted "
        "INTEGER NOT NULL DEFAULT 0;"
    );
}

void create_identity_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE IF NOT EXISTS identity ("
        "singleton INTEGER PRIMARY KEY NOT NULL CHECK (singleton = 1),"
        "user_id TEXT NOT NULL,"
        "user_display_name TEXT NOT NULL,"
        "drive_id TEXT NOT NULL,"
        "drive_name TEXT NOT NULL,"
        "avatar_content_type TEXT NOT NULL,"
        "avatar_bytes BLOB NOT NULL"
        ");"
    );
}

void create_drive_mapping_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE IF NOT EXISTS drive_mapping ("
        "configured_drive_id TEXT PRIMARY KEY NOT NULL,"
        "canonical_drive_id TEXT NOT NULL,"
        "last_resolved INTEGER NOT NULL DEFAULT (unixepoch())"
        ");"
    );
}

void create_partial_download_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE IF NOT EXISTS partial_download ("
        "drive_id TEXT NOT NULL,"
        "remote_id TEXT NOT NULL,"
        "parent_id TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "etag TEXT NOT NULL,"
        "ctag TEXT NOT NULL DEFAULT '',"
        "remote_path TEXT NOT NULL,"
        "local_path TEXT NOT NULL,"
        "last_modified TEXT NOT NULL,"
        "size INTEGER NOT NULL,"
        "directory INTEGER NOT NULL,"
        "temporary_path TEXT NOT NULL,"
        "completed_bytes INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL DEFAULT (unixepoch()),"
        "PRIMARY KEY (drive_id, remote_id)"
        ");"
    );
}

void create_partial_download_v8_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE IF NOT EXISTS partial_download ("
        "drive_id TEXT NOT NULL,"
        "remote_id TEXT NOT NULL,"
        "parent_id TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "etag TEXT NOT NULL,"
        "remote_path TEXT NOT NULL,"
        "local_path TEXT NOT NULL,"
        "last_modified TEXT NOT NULL,"
        "size INTEGER NOT NULL,"
        "directory INTEGER NOT NULL,"
        "temporary_path TEXT NOT NULL,"
        "completed_bytes INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL DEFAULT (unixepoch()),"
        "PRIMARY KEY (drive_id, remote_id)"
        ");"
    );
}

void create_pending_upload_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE IF NOT EXISTS pending_upload ("
        "drive_id TEXT NOT NULL,"
        "remote_path TEXT NOT NULL,"
        "local_path TEXT NOT NULL,"
        "snapshot_path TEXT NOT NULL,"
        "content_fingerprint TEXT NOT NULL,"
        "local_size INTEGER NOT NULL,"
        "local_modified_ticks INTEGER NOT NULL,"
        "remote_id TEXT NOT NULL DEFAULT '',"
        "expected_etag TEXT NOT NULL DEFAULT '',"
        "upload_url TEXT NOT NULL DEFAULT '',"
        "upload_expiration TEXT NOT NULL DEFAULT '',"
        "completed_bytes INTEGER NOT NULL DEFAULT 0,"
        "failure_code TEXT NOT NULL DEFAULT '',"
        "failure_message TEXT NOT NULL DEFAULT '',"
        "failure_attempt_count INTEGER NOT NULL DEFAULT 0,"
        "directory INTEGER NOT NULL DEFAULT 0,"
        "PRIMARY KEY (drive_id, remote_path)"
        ");"
    );
}

void create_pending_upload_v13_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE pending_upload ("
        "drive_id TEXT NOT NULL,"
        "remote_path TEXT NOT NULL,"
        "local_path TEXT NOT NULL,"
        "snapshot_path TEXT NOT NULL,"
        "content_fingerprint TEXT NOT NULL,"
        "local_size INTEGER NOT NULL,"
        "local_modified_ticks INTEGER NOT NULL,"
        "remote_id TEXT NOT NULL DEFAULT '',"
        "expected_etag TEXT NOT NULL DEFAULT '',"
        "PRIMARY KEY (drive_id, remote_path)"
        ");"
    );
}

void create_pending_move_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE IF NOT EXISTS pending_move ("
        "drive_id TEXT NOT NULL,"
        "remote_id TEXT NOT NULL,"
        "source_path TEXT NOT NULL,"
        "destination_path TEXT NOT NULL,"
        "staging_path TEXT NOT NULL DEFAULT '',"
        "source_device INTEGER NOT NULL,"
        "source_inode INTEGER NOT NULL,"
        "directory INTEGER NOT NULL,"
        "PRIMARY KEY (drive_id, remote_id)"
        ");"
    );
}

void create_pending_delete_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE IF NOT EXISTS pending_delete ("
        "drive_id TEXT NOT NULL,"
        "remote_id TEXT NOT NULL,"
        "expected_etag TEXT NOT NULL,"
        "remote_path TEXT NOT NULL,"
        "local_path TEXT NOT NULL,"
        "directory INTEGER NOT NULL,"
        "PRIMARY KEY (drive_id, remote_id)"
        ");"
    );
}

void create_pending_remote_move_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE IF NOT EXISTS pending_remote_move ("
        "drive_id TEXT NOT NULL,"
        "remote_id TEXT NOT NULL,"
        "expected_etag TEXT NOT NULL,"
        "source_remote_path TEXT NOT NULL,"
        "destination_remote_path TEXT NOT NULL,"
        "source_local_path TEXT NOT NULL,"
        "destination_local_path TEXT NOT NULL,"
        "local_device INTEGER NOT NULL,"
        "local_inode INTEGER NOT NULL,"
        "directory INTEGER NOT NULL,"
        "PRIMARY KEY (drive_id, remote_id)"
        ");"
    );
}

void create_pending_move_v14_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE pending_move ("
        "drive_id TEXT NOT NULL,"
        "remote_id TEXT NOT NULL,"
        "source_path TEXT NOT NULL,"
        "destination_path TEXT NOT NULL,"
        "source_device INTEGER NOT NULL,"
        "source_inode INTEGER NOT NULL,"
        "directory INTEGER NOT NULL,"
        "PRIMARY KEY (drive_id, remote_id)"
        ");"
    );
}

void create_upload_suppression_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE IF NOT EXISTS upload_suppression ("
        "drive_id TEXT NOT NULL,"
        "remote_id TEXT NOT NULL,"
        "local_path TEXT NOT NULL,"
        "source_device INTEGER NOT NULL,"
        "source_inode INTEGER NOT NULL,"
        "PRIMARY KEY (drive_id, local_path)"
        ");"
    );
}

constexpr int current_schema_version = 24;

void set_schema_version(sqlite3* database, int version) {
    const auto sql =
        "PRAGMA user_version = " + std::to_string(version) + ";";
    execute(database, sql.c_str());
}

void ensure_current_schema(sqlite3* database) {
    create_item_schema(database);
    create_drive_state_schema(database);
    create_pending_download_schema(database);
    create_blocked_item_schema(database);
    create_identity_schema(database);
    create_drive_mapping_schema(database);
    create_partial_download_schema(database);
    create_pending_upload_schema(database);
    create_pending_delete_schema(database);
    create_pending_remote_move_schema(database);
    create_pending_move_schema(database);
    create_upload_suppression_schema(database);
    set_schema_version(database, current_schema_version);
}

std::string quote_identifier(std::string_view identifier) {
    std::string quoted{"\""};
    for (const char character : identifier) {
        quoted.push_back(character);
        if (character == '"') {
            quoted.push_back('"');
        }
    }
    quoted.push_back('"');
    return quoted;
}

std::vector<std::string> user_tables(sqlite3* database) {
    Statement statement{
        database,
        "SELECT name FROM sqlite_schema "
        "WHERE type = 'table' AND name NOT LIKE 'sqlite_%' "
        "ORDER BY name;"
    };
    std::vector<std::string> tables;
    for (int result = sqlite3_step(statement.get());
         result != SQLITE_DONE;
         result = sqlite3_step(statement.get())) {
        if (result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot inspect state database tables: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        tables.push_back(column_text(statement.get(), 0));
    }
    return tables;
}

struct ColumnShape {
    std::string name;
    std::string type;
    int not_null;
    int primary_key_position;

    auto operator<=>(const ColumnShape&) const = default;
};

std::vector<ColumnShape> table_columns(
    sqlite3* database,
    const std::string& table
) {
    const auto sql = "PRAGMA table_info(" + quote_identifier(table) + ");";
    Statement statement{database, sql.c_str()};
    std::vector<ColumnShape> columns;
    for (int result = sqlite3_step(statement.get());
         result != SQLITE_DONE;
         result = sqlite3_step(statement.get())) {
        if (result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot inspect columns for state database table '" + table +
                "': " + sqlite3_errmsg(database)
            );
        }
        columns.push_back({
            .name = column_text(statement.get(), 1),
            .type = column_text(statement.get(), 2),
            .not_null = sqlite3_column_int(statement.get(), 3),
            .primary_key_position = sqlite3_column_int(statement.get(), 5),
        });
    }
    std::ranges::sort(columns, {}, &ColumnShape::name);
    return columns;
}

struct IndexShape {
    std::vector<std::string> columns;
    int unique;
    std::string origin;
    int partial;

    auto operator<=>(const IndexShape&) const = default;
};

std::vector<IndexShape> table_indexes(
    sqlite3* database,
    const std::string& table
) {
    const auto list_sql =
        "PRAGMA index_list(" + quote_identifier(table) + ");";
    Statement list{database, list_sql.c_str()};
    std::vector<IndexShape> indexes;
    for (int result = sqlite3_step(list.get());
         result != SQLITE_DONE;
         result = sqlite3_step(list.get())) {
        if (result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot inspect indexes for state database table '" + table +
                "': " + sqlite3_errmsg(database)
            );
        }
        const auto index_name = column_text(list.get(), 1);
        const auto info_sql =
            "PRAGMA index_info(" + quote_identifier(index_name) + ");";
        Statement info{database, info_sql.c_str()};
        IndexShape index{
            .columns = {},
            .unique = sqlite3_column_int(list.get(), 2),
            .origin = column_text(list.get(), 3),
            .partial = sqlite3_column_int(list.get(), 4),
        };
        for (int info_result = sqlite3_step(info.get());
             info_result != SQLITE_DONE;
             info_result = sqlite3_step(info.get())) {
            if (info_result != SQLITE_ROW) {
                throw std::runtime_error(
                    "cannot inspect state database index '" + index_name +
                    "': " + sqlite3_errmsg(database)
                );
            }
            index.columns.push_back(column_text(info.get(), 2));
        }
        indexes.push_back(std::move(index));
    }
    std::ranges::sort(indexes);
    return indexes;
}

void verify_database_integrity(sqlite3* database) {
    try {
        Statement statement{database, "PRAGMA quick_check;"};
        if (sqlite3_step(statement.get()) != SQLITE_ROW) {
            throw DatabaseCorruption(
                "SQLite quick_check could not inspect the state database: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        const auto result = column_text(statement.get(), 0);
        if (result != "ok") {
            throw DatabaseCorruption(
                "SQLite quick_check reported state database corruption: " +
                result
            );
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw DatabaseCorruption(
                "SQLite quick_check returned unexpected additional results"
            );
        }
    } catch (const DatabaseCorruption&) {
        throw;
    } catch (const std::exception& error) {
        const int code = sqlite3_errcode(database) & 0xff;
        if (code == SQLITE_CORRUPT || code == SQLITE_NOTADB) {
            throw DatabaseCorruption(error.what());
        }
        throw;
    }
}

std::string full_integrity_result(sqlite3* database) {
    Statement integrity{database, "PRAGMA integrity_check;"};
    std::string detail;
    while (true) {
        const int result = sqlite3_step(integrity.get());
        if (result == SQLITE_DONE) {
            break;
        }
        if (result != SQLITE_ROW) {
            return "SQLite integrity_check failed: " +
                   std::string{sqlite3_errmsg(database)};
        }
        const auto message = column_text(integrity.get(), 0);
        if (message != "ok") {
            if (!detail.empty()) {
                detail += "; ";
            }
            detail += message;
        }
    }

    Statement foreign_keys{database, "PRAGMA foreign_key_check;"};
    if (sqlite3_step(foreign_keys.get()) != SQLITE_DONE) {
        if (!detail.empty()) {
            detail += "; ";
        }
        detail += "foreign key constraint violation";
    }
    return detail.empty() ? "ok" : detail;
}

void configure_writable_database(sqlite3* database) {
    execute(database, "PRAGMA journal_mode = WAL;");
    execute(database, "PRAGMA synchronous = FULL;");
    execute(database, "PRAGMA secure_delete = FAST;");
    execute(database, "PRAGMA wal_autocheckpoint = 1000;");
    const auto maximum_pages =
        "PRAGMA max_page_count = " + std::to_string(maximum_database_pages) +
        ";";
    execute(database, maximum_pages.c_str());
    require_pragma_value(
        database, "PRAGMA journal_mode;", "wal", "WAL journal mode"
    );
    require_pragma_value(
        database, "PRAGMA synchronous;", "2", "full synchronous writes"
    );
    require_pragma_value(
        database, "PRAGMA secure_delete;", "2", "fast secure deletion"
    );
    require_pragma_value(
        database,
        "PRAGMA wal_autocheckpoint;",
        "1000",
        "automatic WAL checkpoints"
    );
    require_pragma_value(
        database,
        "PRAGMA max_page_count;",
        std::to_string(maximum_database_pages),
        "database page limit"
    );
}

std::filesystem::path
quarantine_corrupt_database(const std::filesystem::path& database_path) {
    const auto timestamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        )
            .count();
    std::filesystem::path quarantine_path{
        database_path.string() + ".corrupt-" + std::to_string(timestamp)
    };
    for (std::size_t suffix = 1; std::filesystem::exists(quarantine_path);
         ++suffix) {
        quarantine_path = database_path.string() + ".corrupt-" +
                          std::to_string(timestamp) + "-" +
                          std::to_string(suffix);
    }

    const std::array sidecars{
        std::pair{
            std::filesystem::path{database_path.string() + "-wal"},
            std::filesystem::path{quarantine_path.string() + "-wal"},
        },
        std::pair{
            std::filesystem::path{database_path.string() + "-shm"},
            std::filesystem::path{quarantine_path.string() + "-shm"},
        },
    };
    for (const auto& [source, destination] : sidecars) {
        if (std::filesystem::exists(source)) {
            std::filesystem::rename(source, destination);
        }
    }
    std::filesystem::rename(database_path, quarantine_path);
    spdlog::error(
        "Quarantined corrupt synchronization state database as '{}'",
        quarantine_path.string()
    );
    return quarantine_path;
}

void verify_current_schema(sqlite3* database) {
    sqlite3* reference_handle = nullptr;
    const int result = sqlite3_open(":memory:", &reference_handle);
    SqliteHandle reference{reference_handle};
    if (result != SQLITE_OK) {
        const std::string message =
            reference == nullptr ?
                "unknown SQLite error" :
                sqlite3_errmsg(reference.get());
        throw std::runtime_error(
            "cannot create reference state database schema: " + message
        );
    }
    ensure_current_schema(reference.get());

    const auto expected_tables = user_tables(reference.get());
    const auto actual_tables = user_tables(database);
    for (const auto& expected : expected_tables) {
        if (!std::ranges::contains(actual_tables, expected)) {
            throw std::runtime_error(
                "state database schema is missing table '" + expected + "'"
            );
        }
    }
    for (const auto& actual : actual_tables) {
        if (!std::ranges::contains(expected_tables, actual)) {
            throw std::runtime_error(
                "state database schema contains unexpected table '" + actual +
                "'"
            );
        }
    }

    for (const auto& table : expected_tables) {
        const auto expected_columns = table_columns(reference.get(), table);
        const auto actual_columns = table_columns(database, table);
        for (const auto& expected : expected_columns) {
            const auto actual = std::ranges::find(
                actual_columns,
                expected.name,
                &ColumnShape::name
            );
            if (actual == actual_columns.end()) {
                throw std::runtime_error(
                    "state database table '" + table +
                    "' is missing column '" + expected.name + "'"
                );
            }
            if (*actual != expected) {
                throw std::runtime_error(
                    "state database table '" + table + "' column '" +
                    expected.name + "' has an incompatible definition"
                );
            }
        }
        for (const auto& actual : actual_columns) {
            if (!std::ranges::contains(
                    expected_columns,
                    actual.name,
                    &ColumnShape::name
                )) {
                throw std::runtime_error(
                    "state database table '" + table +
                    "' contains unexpected column '" + actual.name + "'"
                );
            }
        }
        if (table_indexes(database, table) !=
            table_indexes(reference.get(), table)) {
            throw std::runtime_error(
                "state database table '" + table +
                "' has incompatible primary key or index definitions"
            );
        }
    }
}

void add_sync_filter_fingerprint(sqlite3* database) {
    execute(
        database,
        "ALTER TABLE drive_state ADD COLUMN sync_filter_fingerprint "
        "TEXT NOT NULL DEFAULT '';"
    );
}

void add_pending_download_backup(sqlite3* database) {
    execute(
        database,
        "ALTER TABLE pending_download ADD COLUMN backup_path "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE pending_download ADD COLUMN backup_fingerprint "
        "TEXT NOT NULL DEFAULT '';"
    );
}

void add_pending_move_staging(sqlite3* database) {
    execute(
        database,
        "ALTER TABLE pending_move ADD COLUMN staging_path "
        "TEXT NOT NULL DEFAULT '';"
    );
}

void add_pending_upload_session(sqlite3* database) {
    execute(
        database,
        "ALTER TABLE pending_upload ADD COLUMN upload_url "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE pending_upload ADD COLUMN upload_expiration "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE pending_upload ADD COLUMN completed_bytes "
        "INTEGER NOT NULL DEFAULT 0;"
    );
}

void add_pending_upload_directory(sqlite3* database) {
    execute(
        database,
        "ALTER TABLE pending_upload ADD COLUMN directory "
        "INTEGER NOT NULL DEFAULT 0;"
    );
}

void add_item_local_identity(sqlite3* database) {
    execute(
        database,
        "ALTER TABLE item ADD COLUMN local_device "
        "INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE item ADD COLUMN local_inode "
        "INTEGER NOT NULL DEFAULT 0;"
    );
}

void add_content_tags(sqlite3* database) {
    execute(
        database,
        "ALTER TABLE item ADD COLUMN ctag TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE pending_download ADD COLUMN ctag "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE partial_download ADD COLUMN ctag "
        "TEXT NOT NULL DEFAULT '';"
    );
}

void add_pending_upload_failure(sqlite3* database) {
    execute(
        database,
        "ALTER TABLE pending_upload ADD COLUMN failure_code "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE pending_upload ADD COLUMN failure_message "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE pending_upload ADD COLUMN failure_attempt_count "
        "INTEGER NOT NULL DEFAULT 0;"
    );
}

void add_blocked_item_content_tag(sqlite3* database) {
    execute(
        database,
        "ALTER TABLE blocked_item ADD COLUMN ctag TEXT NOT NULL DEFAULT '';"
    );
}

void migrate_v1_to_v4(sqlite3* database) {
    execute(
        database,
        "ALTER TABLE item RENAME TO item_v1;"
        "CREATE TABLE item ("
        "drive_id TEXT NOT NULL,"
        "remote_id TEXT NOT NULL,"
        "parent_id TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "etag TEXT NOT NULL,"
        "remote_path TEXT NOT NULL,"
        "local_path TEXT NOT NULL,"
        "last_modified TEXT NOT NULL,"
        "size INTEGER NOT NULL,"
        "local_size INTEGER NOT NULL,"
        "local_modified_ticks INTEGER NOT NULL,"
        "directory INTEGER NOT NULL,"
        "PRIMARY KEY (drive_id, remote_id)"
        ");"
        "INSERT INTO item ("
        "drive_id, remote_id, parent_id, name, etag, remote_path, "
        "local_path, last_modified, size, local_size, "
        "local_modified_ticks, directory"
        ") SELECT '', remote_id, '', '', etag, local_path, local_path, '', "
        "0, 0, 0, 0 FROM item_v1;"
        "DROP TABLE item_v1;"
        "CREATE TABLE drive_state ("
        "drive_id TEXT PRIMARY KEY NOT NULL,"
        "delta_link TEXT NOT NULL"
        ");"
        "CREATE TABLE pending_download ("
        "drive_id TEXT NOT NULL,"
        "remote_id TEXT NOT NULL,"
        "parent_id TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "etag TEXT NOT NULL,"
        "remote_path TEXT NOT NULL,"
        "local_path TEXT NOT NULL,"
        "last_modified TEXT NOT NULL,"
        "size INTEGER NOT NULL,"
        "directory INTEGER NOT NULL,"
        "temporary_path TEXT NOT NULL,"
        "content_fingerprint TEXT NOT NULL,"
        "PRIMARY KEY (drive_id, remote_id)"
        ");"
    );
}

void migrate_v2_to_v4(sqlite3* database) {
    execute(
        database,
        "ALTER TABLE item ADD COLUMN local_size INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE item ADD COLUMN local_modified_ticks INTEGER NOT NULL "
        "DEFAULT 0;"
        "CREATE TABLE pending_download ("
        "drive_id TEXT NOT NULL,"
        "remote_id TEXT NOT NULL,"
        "parent_id TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "etag TEXT NOT NULL,"
        "remote_path TEXT NOT NULL,"
        "local_path TEXT NOT NULL,"
        "last_modified TEXT NOT NULL,"
        "size INTEGER NOT NULL,"
        "directory INTEGER NOT NULL,"
        "temporary_path TEXT NOT NULL,"
        "content_fingerprint TEXT NOT NULL,"
        "PRIMARY KEY (drive_id, remote_id)"
        ");"
    );
}

void migrate_v3_to_v4(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE pending_download ("
        "drive_id TEXT NOT NULL,"
        "remote_id TEXT NOT NULL,"
        "parent_id TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "etag TEXT NOT NULL,"
        "remote_path TEXT NOT NULL,"
        "local_path TEXT NOT NULL,"
        "last_modified TEXT NOT NULL,"
        "size INTEGER NOT NULL,"
        "directory INTEGER NOT NULL,"
        "temporary_path TEXT NOT NULL,"
        "content_fingerprint TEXT NOT NULL,"
        "PRIMARY KEY (drive_id, remote_id)"
        ");"
    );
}

struct SchemaMigration {
    int from_version;
    int to_version;
    void (*apply)(sqlite3*);
};

constexpr std::array schema_migrations{
    SchemaMigration{1, 4, migrate_v1_to_v4},
    SchemaMigration{2, 4, migrate_v2_to_v4},
    SchemaMigration{3, 4, migrate_v3_to_v4},
    SchemaMigration{4, 5, create_blocked_item_v5_schema},
    SchemaMigration{5, 6, create_identity_schema},
    SchemaMigration{6, 7, create_drive_mapping_schema},
    SchemaMigration{7, 8, create_partial_download_v8_schema},
    SchemaMigration{8, 9, add_blocked_item_hash_columns},
    SchemaMigration{9, 10, add_sync_filter_fingerprint},
    SchemaMigration{10, 11, add_pending_download_backup},
    SchemaMigration{11, 12, add_blocked_item_deleted_column},
    SchemaMigration{12, 13, create_pending_upload_v13_schema},
    SchemaMigration{13, 14, create_pending_move_v14_schema},
    SchemaMigration{14, 15, add_pending_move_staging},
    SchemaMigration{15, 16, create_upload_suppression_schema},
    SchemaMigration{16, 17, add_pending_upload_session},
    SchemaMigration{17, 18, add_pending_upload_directory},
    SchemaMigration{18, 19, create_pending_delete_schema},
    SchemaMigration{19, 20, add_item_local_identity},
    SchemaMigration{20, 21, create_pending_remote_move_schema},
    SchemaMigration{21, 22, add_content_tags},
    SchemaMigration{22, 23, add_pending_upload_failure},
    SchemaMigration{23, 24, add_blocked_item_content_tag},
};

consteval bool schema_migration_chain_is_complete() {
    for (int version = 1; version < current_schema_version; ++version) {
        int matches = 0;
        for (const auto& migration : schema_migrations) {
            if (migration.from_version == version) {
                ++matches;
                if (migration.to_version <= migration.from_version ||
                    migration.to_version > current_schema_version) {
                    return false;
                }
            }
        }
        if (matches != 1) {
            return false;
        }
    }
    return true;
}

static_assert(schema_migration_chain_is_complete());

void migrate_schema(sqlite3* database) {
    int version = schema_version(database);
    if (version == 0) {
        if (!user_tables(database).empty()) {
            throw std::runtime_error(
                "unversioned state database contains existing tables"
            );
        }
        ensure_current_schema(database);
        return;
    }
    if (version < 0 || version > current_schema_version) {
        throw std::runtime_error(
            "unsupported state database schema version " +
            std::to_string(version)
        );
    }

    while (version < current_schema_version) {
        const SchemaMigration* migration = nullptr;
        for (const auto& candidate : schema_migrations) {
            if (candidate.from_version == version) {
                migration = &candidate;
                break;
            }
        }
        if (migration == nullptr) {
            throw std::runtime_error(
                "no state database migration from schema version " +
                std::to_string(version)
            );
        }

        Transaction transaction{database};
        migration->apply(database);
        set_schema_version(database, migration->to_version);
        transaction.commit();
        version = migration->to_version;
    }

}

}  // namespace onedrive::storage::item_database_detail
