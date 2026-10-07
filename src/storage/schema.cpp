#include "storage/schema.hpp"
#include "storage/schema_internal.hpp"

#include "storage/sqlite.hpp"

#include <array>
#include <stdexcept>
#include <string>

namespace onedrive::storage::item_database_detail {

int schema_version(sqlite3* database) {
    Statement statement{database, "PRAGMA user_version;"};
    statement.require_row("cannot read state database schema version");
    return sqlite3_column_int(statement.get(), 0);
}

void create_item_schema(sqlite3* database) {
    execute_sql(
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
        "content_hash_algorithm TEXT NOT NULL DEFAULT '',"
        "content_hash_value TEXT NOT NULL DEFAULT '',"
        "directory INTEGER NOT NULL,"
        "PRIMARY KEY (drive_id, remote_id)"
        ");"
    );
}

void create_drive_state_schema(sqlite3* database) {
    execute_sql(
        database,
        "CREATE TABLE IF NOT EXISTS drive_state ("
        "drive_id TEXT PRIMARY KEY NOT NULL,"
        "delta_link TEXT NOT NULL,"
        "sync_filter_fingerprint TEXT NOT NULL DEFAULT ''"
        ");"
    );
}

void create_pending_download_schema(sqlite3* database) {
    execute_sql(
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
    execute_sql(
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
    execute_sql(
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
    execute_sql(
        database,
        "ALTER TABLE blocked_item ADD COLUMN content_hash_algorithm "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE blocked_item ADD COLUMN content_hash_value "
        "TEXT NOT NULL DEFAULT '';"
    );
}

void add_blocked_item_deleted_column(sqlite3* database) {
    execute_sql(
        database,
        "ALTER TABLE blocked_item ADD COLUMN deleted "
        "INTEGER NOT NULL DEFAULT 0;"
    );
}

void add_item_content_hash(sqlite3* database) {
    execute_sql(
        database,
        "ALTER TABLE item ADD COLUMN content_hash_algorithm "
        "TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE item ADD COLUMN content_hash_value "
        "TEXT NOT NULL DEFAULT '';"
    );
}

void create_identity_schema(sqlite3* database) {
    execute_sql(
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
    execute_sql(
        database,
        "CREATE TABLE IF NOT EXISTS drive_mapping ("
        "configured_drive_id TEXT PRIMARY KEY NOT NULL,"
        "canonical_drive_id TEXT NOT NULL,"
        "last_resolved INTEGER NOT NULL DEFAULT (unixepoch())"
        ");"
    );
}

void create_partial_download_schema(sqlite3* database) {
    execute_sql(
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
    execute_sql(
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
    execute_sql(
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
    execute_sql(
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
    execute_sql(
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
    execute_sql(
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
    execute_sql(
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
    execute_sql(
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
    execute_sql(
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

void set_schema_version(sqlite3* database, int version) {
    const auto sql = "PRAGMA user_version = " + std::to_string(version) + ";";
    execute_sql(database, sql.c_str());
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

} // namespace onedrive::storage::item_database_detail
