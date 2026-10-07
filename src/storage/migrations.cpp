#include "storage/schema.hpp"
#include "storage/schema_internal.hpp"
#include "storage/sqlite.hpp"

#include <array>
#include <stdexcept>
#include <string>

namespace onedrive::storage::item_database_detail {

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
    SchemaMigration{24, 25, add_item_content_hash},
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
