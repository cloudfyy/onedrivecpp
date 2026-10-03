#include "onedrive/storage/item_database.hpp"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>

namespace onedrive::storage {
namespace {

class Statement {
public:
    Statement(sqlite3* database, const char* sql) : database_{database} {
        const int result = sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr);
        if (result != SQLITE_OK) {
            throw std::runtime_error(
                "cannot prepare SQLite statement: " + std::string{sqlite3_errmsg(database_)}
            );
        }
    }

    ~Statement() {
        sqlite3_finalize(statement_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept {
        return statement_;
    }

private:
    sqlite3* database_;
    sqlite3_stmt* statement_{nullptr};
};

void execute(sqlite3* database, const char* sql) {
    char* error_message = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error_message);
    if (result == SQLITE_OK) {
        return;
    }

    const std::string message =
        error_message == nullptr ? sqlite3_errmsg(database) : error_message;
    sqlite3_free(error_message);
    throw std::runtime_error("SQLite operation failed: " + message);
}

void bind_text(sqlite3* database, sqlite3_stmt* statement, int index, const std::string& value) {
    const int result = sqlite3_bind_text(
        statement,
        index,
        value.c_str(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT
    );
    if (result != SQLITE_OK) {
        throw std::runtime_error(
            "cannot bind SQLite value: " + std::string{sqlite3_errmsg(database)}
        );
    }
}

void bind_integer(
    sqlite3* database,
    sqlite3_stmt* statement,
    int index,
    std::int64_t value
) {
    if (sqlite3_bind_int64(statement, index, value) != SQLITE_OK) {
        throw std::runtime_error(
            "cannot bind SQLite integer: " + std::string{sqlite3_errmsg(database)}
        );
    }
}

void bind_blob(
    sqlite3* database,
    sqlite3_stmt* statement,
    int index,
    const std::vector<std::uint8_t>& value
) {
    const int result =
        value.empty() ?
            sqlite3_bind_zeroblob64(statement, index, 0) :
            sqlite3_bind_blob64(
                statement,
                index,
                value.data(),
                static_cast<sqlite3_uint64>(value.size()),
                SQLITE_TRANSIENT
            );
    if (result != SQLITE_OK) {
        throw std::runtime_error(
            "cannot bind SQLite blob: " + std::string{sqlite3_errmsg(database)}
        );
    }
}

std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value == nullptr ? std::string{} :
                              std::string{reinterpret_cast<const char*>(value)};
}

std::string item_key(std::string_view drive_id, std::string_view remote_id) {
    std::string key;
    key.reserve(drive_id.size() + remote_id.size() + 1);
    key.append(drive_id);
    key.push_back('\0');
    key.append(remote_id);
    return key;
}

class Transaction {
public:
    explicit Transaction(sqlite3* database) : database_{database} {
        execute(database_, "BEGIN IMMEDIATE;");
    }

    ~Transaction() {
        if (!committed_) {
            sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit() {
        execute(database_, "COMMIT;");
        committed_ = true;
    }

private:
    sqlite3* database_;
    bool committed_{false};
};

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

void create_blocked_item_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE IF NOT EXISTS blocked_item ("
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
        "CREATE TABLE IF NOT EXISTS drive_mapping ("
        "configured_drive_id TEXT PRIMARY KEY NOT NULL,"
        "canonical_drive_id TEXT NOT NULL,"
        "last_resolved INTEGER NOT NULL DEFAULT (unixepoch())"
        ");"
    );
}

void create_current_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE IF NOT EXISTS item ("
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
        "CREATE TABLE IF NOT EXISTS drive_state ("
        "drive_id TEXT PRIMARY KEY NOT NULL,"
        "delta_link TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS pending_download ("
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
    create_blocked_item_schema(database);
    create_identity_schema(database);
    execute(database, "PRAGMA user_version = 7;");
}

void migrate_schema(sqlite3* database) {
    const int version = schema_version(database);
    if (version == 0) {
        create_current_schema(database);
        return;
    }
    if (version == 1) {
        Transaction transaction{database};
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
            "PRAGMA user_version = 4;"
        );
        create_blocked_item_schema(database);
        create_identity_schema(database);
        execute(database, "PRAGMA user_version = 7;");
        transaction.commit();
        return;
    }
    if (version == 2) {
        Transaction transaction{database};
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
            "PRAGMA user_version = 4;"
        );
        create_blocked_item_schema(database);
        create_identity_schema(database);
        execute(database, "PRAGMA user_version = 7;");
        transaction.commit();
        return;
    }
    if (version == 3) {
        Transaction transaction{database};
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
            "PRAGMA user_version = 4;"
        );
        create_blocked_item_schema(database);
        create_identity_schema(database);
        execute(database, "PRAGMA user_version = 7;");
        transaction.commit();
        return;
    }
    if (version == 4) {
        Transaction transaction{database};
        create_blocked_item_schema(database);
        create_identity_schema(database);
        execute(database, "PRAGMA user_version = 7;");
        transaction.commit();
        return;
    }
    if (version == 5) {
        Transaction transaction{database};
        create_identity_schema(database);
        execute(database, "PRAGMA user_version = 7;");
        transaction.commit();
        return;
    }
    if (version == 6) {
        Transaction transaction{database};
        create_identity_schema(database);
        execute(database, "PRAGMA user_version = 7;");
        transaction.commit();
        return;
    }
    if (version != 7) {
        throw std::runtime_error(
            "unsupported state database schema version " + std::to_string(version)
        );
    }
    create_current_schema(database);
}

}  // namespace

struct ItemDatabase::Impl {
    struct DatabaseCloser {
        void operator()(sqlite3* handle) const noexcept {
            sqlite3_close(handle);
        }
    };

    std::unique_ptr<sqlite3, DatabaseCloser> database;
};

ItemDatabase::ItemDatabase(
    std::filesystem::path state_directory,
    account::DriveIdentity identity
)
    : state_directory_{std::move(state_directory)},
      identity_{std::move(identity)},
      impl_{std::make_unique<Impl>()} {}

ItemDatabase::~ItemDatabase() = default;

void ItemDatabase::open() {
    spdlog::debug("Opening synchronization state database");
    if (identity_.user_id.empty() || identity_.user_display_name.empty() ||
        identity_.configured_drive_id.empty() || identity_.drive_id.empty() ||
        identity_.drive_name.empty()) {
        throw std::invalid_argument(
            "state database requires a complete account and drive identity"
        );
    }
    std::filesystem::create_directories(state_directory_);
    items_.clear();
    blocked_items_.clear();
    delta_links_.clear();
    impl_->database.reset();

    sqlite3* database = nullptr;
    const auto database_path = state_directory_ / "items.sqlite3";
    const int result = sqlite3_open_v2(
        database_path.string().c_str(),
        &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW,
        nullptr
    );
    if (result != SQLITE_OK) {
        const std::string message =
            database == nullptr ? "unknown SQLite error" : sqlite3_errmsg(database);
        sqlite3_close(database);
        throw std::runtime_error(
            "cannot open state database '" + database_path.string() + "': " + message
        );
    }
    impl_->database.reset(database);
    if (::chmod(database_path.c_str(), S_IRUSR | S_IWUSR) == -1) {
        throw std::runtime_error(
            "cannot secure state database '" + database_path.string() + "': " +
            std::strerror(errno)
        );
    }

    execute(database, "PRAGMA journal_mode = WAL;");
    migrate_schema(database);

    Statement identity_query{
        database,
        "SELECT user_id, drive_id FROM identity WHERE singleton = 1;"
    };
    const int identity_result = sqlite3_step(identity_query.get());
    if (identity_result == SQLITE_ROW &&
        (column_text(identity_query.get(), 0) != identity_.user_id ||
         column_text(identity_query.get(), 1) != identity_.drive_id)) {
        throw std::runtime_error(
            "state database identity does not match the current Microsoft "
            "account and drive"
        );
    }
    if (identity_result != SQLITE_ROW && identity_result != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot read state database identity: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    const std::string avatar_content_type =
        identity_.photo ? identity_.photo->content_type : "";
    const std::vector<std::uint8_t> avatar_bytes =
        identity_.photo ? identity_.photo->bytes : std::vector<std::uint8_t>{};
    Statement identity_upsert{
        database,
        "INSERT INTO identity ("
        "singleton, user_id, user_display_name, drive_id, drive_name, "
        "avatar_content_type, avatar_bytes"
        ") VALUES (1, ?1, ?2, ?3, ?4, ?5, ?6) "
        "ON CONFLICT(singleton) DO UPDATE SET "
        "user_display_name = excluded.user_display_name, "
        "drive_name = excluded.drive_name, "
        "avatar_content_type = CASE WHEN length(excluded.avatar_bytes) > 0 "
        "THEN excluded.avatar_content_type ELSE identity.avatar_content_type END, "
        "avatar_bytes = CASE WHEN length(excluded.avatar_bytes) > 0 "
        "THEN excluded.avatar_bytes ELSE identity.avatar_bytes END;"
    };
    bind_text(database, identity_upsert.get(), 1, identity_.user_id);
    bind_text(
        database,
        identity_upsert.get(),
        2,
        identity_.user_display_name
    );
    bind_text(database, identity_upsert.get(), 3, identity_.drive_id);
    bind_text(database, identity_upsert.get(), 4, identity_.drive_name);
    bind_text(database, identity_upsert.get(), 5, avatar_content_type);
    bind_blob(database, identity_upsert.get(), 6, avatar_bytes);
    if (sqlite3_step(identity_upsert.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot update state database identity: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    Statement mapping_upsert{
        database,
        "INSERT INTO drive_mapping ("
        "configured_drive_id, canonical_drive_id"
        ") VALUES (?1, ?2) "
        "ON CONFLICT(configured_drive_id) DO UPDATE SET "
        "canonical_drive_id = excluded.canonical_drive_id, "
        "last_resolved = unixepoch();"
    };
    bind_text(
        database,
        mapping_upsert.get(),
        1,
        identity_.configured_drive_id
    );
    bind_text(database, mapping_upsert.get(), 2, identity_.drive_id);
    if (sqlite3_step(mapping_upsert.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot update configured Drive ID mapping: " +
            std::string{sqlite3_errmsg(database)}
        );
    }

    Statement query{
        database,
        "SELECT drive_id, remote_id, parent_id, name, etag, remote_path, "
        "local_path, last_modified, size, local_size, local_modified_ticks, "
        "directory "
        "FROM item ORDER BY drive_id, remote_id;"
    };
    while (true) {
        const int step_result = sqlite3_step(query.get());
        if (step_result == SQLITE_DONE) {
            break;
        }
        if (step_result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read state database: " + std::string{sqlite3_errmsg(database)}
            );
        }

        ItemState item{
            .drive_id = column_text(query.get(), 0),
            .remote_id = column_text(query.get(), 1),
            .parent_id = column_text(query.get(), 2),
            .name = column_text(query.get(), 3),
            .etag = column_text(query.get(), 4),
            .remote_path = column_text(query.get(), 5),
            .local_path = column_text(query.get(), 6),
            .last_modified = column_text(query.get(), 7),
            .size = sqlite3_column_int64(query.get(), 8),
            .local_size = sqlite3_column_int64(query.get(), 9),
            .local_modified_ticks = sqlite3_column_int64(query.get(), 10),
            .directory = sqlite3_column_int(query.get(), 11) != 0,
        };
        const std::string key = item_key(item.drive_id, item.remote_id);
        items_.insert_or_assign(key, std::move(item));
    }

    Statement blocked_query{
        database,
        "SELECT drive_id, remote_id, parent_id, name, etag, remote_path, "
        "last_modified, size, directory, reason_code, reason_message, "
        "attempt_count FROM blocked_item ORDER BY drive_id, remote_id;"
    };
    while (true) {
        const int step_result = sqlite3_step(blocked_query.get());
        if (step_result == SQLITE_DONE) {
            break;
        }
        if (step_result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read blocked synchronization items: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        BlockedItem item{
            .drive_id = column_text(blocked_query.get(), 0),
            .remote_id = column_text(blocked_query.get(), 1),
            .parent_id = column_text(blocked_query.get(), 2),
            .name = column_text(blocked_query.get(), 3),
            .etag = column_text(blocked_query.get(), 4),
            .remote_path = column_text(blocked_query.get(), 5),
            .last_modified = column_text(blocked_query.get(), 6),
            .size = sqlite3_column_int64(blocked_query.get(), 7),
            .directory = sqlite3_column_int(blocked_query.get(), 8) != 0,
            .reason_code = column_text(blocked_query.get(), 9),
            .reason_message = column_text(blocked_query.get(), 10),
            .attempt_count = static_cast<std::uint64_t>(
                sqlite3_column_int64(blocked_query.get(), 11)
            ),
        };
        blocked_items_.insert_or_assign(
            item_key(item.drive_id, item.remote_id),
            std::move(item)
        );
    }

    Statement drive_query{
        database,
        "SELECT drive_id, delta_link FROM drive_state ORDER BY drive_id;"
    };
    while (true) {
        const int step_result = sqlite3_step(drive_query.get());
        if (step_result == SQLITE_DONE) {
            break;
        }
        if (step_result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read drive state: " + std::string{sqlite3_errmsg(database)}
            );
        }
        delta_links_.insert_or_assign(
            column_text(drive_query.get(), 0),
            column_text(drive_query.get(), 1)
        );
    }
    spdlog::info(
        "Synchronization state database ready with {} tracked items and {} "
        "blocked items",
        items_.size(),
        blocked_items_.size()
    );
}

void ItemDatabase::upsert(ItemState item) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }

    Statement statement{
        database,
        "INSERT INTO item ("
        "drive_id, remote_id, parent_id, name, etag, remote_path, local_path, "
        "last_modified, size, local_size, local_modified_ticks, directory"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12) "
        "ON CONFLICT(drive_id, remote_id) DO UPDATE SET "
        "parent_id = excluded.parent_id, name = excluded.name, "
        "etag = excluded.etag, remote_path = excluded.remote_path, "
        "local_path = excluded.local_path, "
        "last_modified = excluded.last_modified, size = excluded.size, "
        "local_size = excluded.local_size, "
        "local_modified_ticks = excluded.local_modified_ticks, "
        "directory = excluded.directory;"
    };
    const std::string local_path = item.local_path.string();
    bind_text(database, statement.get(), 1, item.drive_id);
    bind_text(database, statement.get(), 2, item.remote_id);
    bind_text(database, statement.get(), 3, item.parent_id);
    bind_text(database, statement.get(), 4, item.name);
    bind_text(database, statement.get(), 5, item.etag);
    bind_text(database, statement.get(), 6, item.remote_path);
    bind_text(database, statement.get(), 7, local_path);
    bind_text(database, statement.get(), 8, item.last_modified);
    bind_integer(database, statement.get(), 9, item.size);
    bind_integer(database, statement.get(), 10, item.local_size);
    bind_integer(database, statement.get(), 11, item.local_modified_ticks);
    bind_integer(database, statement.get(), 12, item.directory ? 1 : 0);

    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot update state database: " + std::string{sqlite3_errmsg(database)}
        );
    }
    const std::string key = item_key(item.drive_id, item.remote_id);
    items_.insert_or_assign(key, std::move(item));
    spdlog::trace(
        "Updated synchronization state; {} items tracked",
        items_.size()
    );
}

void ItemDatabase::apply_delta(ItemDelta delta) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (delta.drive_id.empty() || delta.delta_link.empty()) {
        throw std::invalid_argument("delta state requires a drive ID and delta link");
    }

    Transaction transaction{database};
    if (delta.replace_drive_items) {
        Statement replace_statement{
            database,
            "DELETE FROM item WHERE drive_id = ?1;"
        };
        bind_text(database, replace_statement.get(), 1, delta.drive_id);
        if (sqlite3_step(replace_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot replace drive items: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        Statement replace_blocked_statement{
            database,
            "DELETE FROM blocked_item WHERE drive_id = ?1;"
        };
        bind_text(
            database,
            replace_blocked_statement.get(),
            1,
            delta.drive_id
        );
        if (sqlite3_step(replace_blocked_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot replace blocked drive items: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
    }
    Statement upsert_statement{
        database,
        "INSERT INTO item ("
        "drive_id, remote_id, parent_id, name, etag, remote_path, local_path, "
        "last_modified, size, local_size, local_modified_ticks, directory"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12) "
        "ON CONFLICT(drive_id, remote_id) DO UPDATE SET "
        "parent_id = excluded.parent_id, name = excluded.name, "
        "etag = excluded.etag, remote_path = excluded.remote_path, "
        "local_path = excluded.local_path, "
        "last_modified = excluded.last_modified, size = excluded.size, "
        "local_size = excluded.local_size, "
        "local_modified_ticks = excluded.local_modified_ticks, "
        "directory = excluded.directory;"
    };
    for (auto& item : delta.upserts) {
        if (item.remote_id.empty()) {
            throw std::invalid_argument("cannot persist a delta item without an ID");
        }
        item.drive_id = delta.drive_id;
        const std::string local_path = item.local_path.string();
        bind_text(database, upsert_statement.get(), 1, item.drive_id);
        bind_text(database, upsert_statement.get(), 2, item.remote_id);
        bind_text(database, upsert_statement.get(), 3, item.parent_id);
        bind_text(database, upsert_statement.get(), 4, item.name);
        bind_text(database, upsert_statement.get(), 5, item.etag);
        bind_text(database, upsert_statement.get(), 6, item.remote_path);
        bind_text(database, upsert_statement.get(), 7, local_path);
        bind_text(database, upsert_statement.get(), 8, item.last_modified);
        bind_integer(database, upsert_statement.get(), 9, item.size);
        bind_integer(database, upsert_statement.get(), 10, item.local_size);
        bind_integer(
            database,
            upsert_statement.get(),
            11,
            item.local_modified_ticks
        );
        bind_integer(database, upsert_statement.get(), 12, item.directory ? 1 : 0);
        if (sqlite3_step(upsert_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot apply delta item: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        sqlite3_reset(upsert_statement.get());
        sqlite3_clear_bindings(upsert_statement.get());
    }

    Statement delete_statement{
        database,
        "DELETE FROM item WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    for (const auto& remote_id : delta.removals) {
        if (remote_id.empty()) {
            throw std::invalid_argument("cannot remove a delta item without an ID");
        }
        bind_text(database, delete_statement.get(), 1, delta.drive_id);
        bind_text(database, delete_statement.get(), 2, remote_id);
        if (sqlite3_step(delete_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot remove delta item: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        sqlite3_reset(delete_statement.get());
        sqlite3_clear_bindings(delete_statement.get());
    }

    Statement delete_blocked_statement{
        database,
        "DELETE FROM blocked_item WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    for (const auto& remote_id : delta.blocked_removals) {
        if (remote_id.empty()) {
            throw std::invalid_argument(
                "cannot remove a blocked item without an ID"
            );
        }
        bind_text(database, delete_blocked_statement.get(), 1, delta.drive_id);
        bind_text(database, delete_blocked_statement.get(), 2, remote_id);
        if (sqlite3_step(delete_blocked_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot remove blocked item: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        sqlite3_reset(delete_blocked_statement.get());
        sqlite3_clear_bindings(delete_blocked_statement.get());
    }

    Statement upsert_blocked_statement{
        database,
        "INSERT INTO blocked_item ("
        "drive_id, remote_id, parent_id, name, etag, remote_path, "
        "last_modified, size, directory, reason_code, reason_message"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11) "
        "ON CONFLICT(drive_id, remote_id) DO UPDATE SET "
        "parent_id = excluded.parent_id, name = excluded.name, "
        "etag = excluded.etag, remote_path = excluded.remote_path, "
        "last_modified = excluded.last_modified, size = excluded.size, "
        "directory = excluded.directory, reason_code = excluded.reason_code, "
        "reason_message = excluded.reason_message, "
        "last_attempt = unixepoch(), "
        "attempt_count = blocked_item.attempt_count + 1;"
    };
    for (auto& item : delta.blocked_upserts) {
        if (item.remote_id.empty() || item.reason_code.empty() ||
            item.reason_message.empty()) {
            throw std::invalid_argument(
                "blocked item requires an ID, reason code, and reason message"
            );
        }
        item.drive_id = delta.drive_id;
        bind_text(database, upsert_blocked_statement.get(), 1, item.drive_id);
        bind_text(database, upsert_blocked_statement.get(), 2, item.remote_id);
        bind_text(database, upsert_blocked_statement.get(), 3, item.parent_id);
        bind_text(database, upsert_blocked_statement.get(), 4, item.name);
        bind_text(database, upsert_blocked_statement.get(), 5, item.etag);
        bind_text(database, upsert_blocked_statement.get(), 6, item.remote_path);
        bind_text(
            database,
            upsert_blocked_statement.get(),
            7,
            item.last_modified
        );
        bind_integer(database, upsert_blocked_statement.get(), 8, item.size);
        bind_integer(
            database,
            upsert_blocked_statement.get(),
            9,
            item.directory ? 1 : 0
        );
        bind_text(
            database,
            upsert_blocked_statement.get(),
            10,
            item.reason_code
        );
        bind_text(
            database,
            upsert_blocked_statement.get(),
            11,
            item.reason_message
        );
        if (sqlite3_step(upsert_blocked_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot persist blocked item: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        sqlite3_reset(upsert_blocked_statement.get());
        sqlite3_clear_bindings(upsert_blocked_statement.get());
    }

    Statement state_statement{
        database,
        "INSERT INTO drive_state (drive_id, delta_link) VALUES (?1, ?2) "
        "ON CONFLICT(drive_id) DO UPDATE SET delta_link = excluded.delta_link;"
    };
    bind_text(database, state_statement.get(), 1, delta.drive_id);
    bind_text(database, state_statement.get(), 2, delta.delta_link);
    if (sqlite3_step(state_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot update delta link: " + std::string{sqlite3_errmsg(database)}
        );
    }
    transaction.commit();

    if (delta.replace_drive_items) {
        for (auto iterator = items_.begin(); iterator != items_.end();) {
            if (iterator->second.drive_id == delta.drive_id) {
                iterator = items_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        for (auto iterator = blocked_items_.begin();
             iterator != blocked_items_.end();) {
            if (iterator->second.drive_id == delta.drive_id) {
                iterator = blocked_items_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }
    for (auto& item : delta.upserts) {
        const std::string key = item_key(delta.drive_id, item.remote_id);
        items_.insert_or_assign(key, std::move(item));
    }
    for (const auto& remote_id : delta.removals) {
        items_.erase(item_key(delta.drive_id, remote_id));
    }
    for (const auto& remote_id : delta.blocked_removals) {
        blocked_items_.erase(item_key(delta.drive_id, remote_id));
    }
    for (auto& item : delta.blocked_upserts) {
        const std::string key = item_key(delta.drive_id, item.remote_id);
        const auto previous = blocked_items_.find(key);
        item.attempt_count =
            previous == blocked_items_.end() ? 1 : previous->second.attempt_count + 1;
        blocked_items_.insert_or_assign(key, std::move(item));
    }
    delta_links_.insert_or_assign(delta.drive_id, std::move(delta.delta_link));
    spdlog::debug(
        "Committed remote delta for drive '{}': {} upserts, {} removals, {} "
        "blocked, inventory {}, delta cursor advanced, {} total items tracked",
        delta.drive_id,
        delta.upserts.size(),
        delta.removals.size(),
        delta.blocked_upserts.size(),
        delta.replace_drive_items ? "replaced" : "updated",
        items_.size()
    );
}

void ItemDatabase::save_pending_download(PendingDownload download) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (download.item.drive_id.empty() || download.item.remote_id.empty() ||
        download.temporary_path.empty() || download.content_fingerprint.empty()) {
        throw std::invalid_argument(
            "pending download requires drive, item, temporary path, and fingerprint"
        );
    }

    Statement statement{
        database,
        "INSERT INTO pending_download ("
        "drive_id, remote_id, parent_id, name, etag, remote_path, local_path, "
        "last_modified, size, directory, temporary_path, content_fingerprint"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12) "
        "ON CONFLICT(drive_id, remote_id) DO UPDATE SET "
        "parent_id = excluded.parent_id, name = excluded.name, "
        "etag = excluded.etag, remote_path = excluded.remote_path, "
        "local_path = excluded.local_path, "
        "last_modified = excluded.last_modified, size = excluded.size, "
        "directory = excluded.directory, temporary_path = excluded.temporary_path, "
        "content_fingerprint = excluded.content_fingerprint;"
    };
    bind_text(database, statement.get(), 1, download.item.drive_id);
    bind_text(database, statement.get(), 2, download.item.remote_id);
    bind_text(database, statement.get(), 3, download.item.parent_id);
    bind_text(database, statement.get(), 4, download.item.name);
    bind_text(database, statement.get(), 5, download.item.etag);
    bind_text(database, statement.get(), 6, download.item.remote_path);
    bind_text(
        database,
        statement.get(),
        7,
        download.item.local_path.string()
    );
    bind_text(database, statement.get(), 8, download.item.last_modified);
    bind_integer(database, statement.get(), 9, download.item.size);
    bind_integer(
        database,
        statement.get(),
        10,
        download.item.directory ? 1 : 0
    );
    bind_text(database, statement.get(), 11, download.temporary_path.string());
    bind_text(database, statement.get(), 12, download.content_fingerprint);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot save pending download: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    spdlog::debug(
        "Saved pending download journal for '{}'",
        download.item.remote_path
    );
}

void ItemDatabase::remove_pending_download(
    const std::string& drive_id,
    const std::string& remote_id
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "DELETE FROM pending_download WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    bind_text(database, statement.get(), 2, remote_id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot remove pending download: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    spdlog::debug(
        "Removed pending download journal for drive '{}', item '{}'",
        drive_id,
        remote_id
    );
}

std::vector<PendingDownload> ItemDatabase::pending_downloads(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_id, parent_id, name, etag, remote_path, "
        "local_path, last_modified, size, directory, temporary_path, "
        "content_fingerprint FROM pending_download WHERE drive_id = ?1 "
        "ORDER BY remote_path;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    std::vector<PendingDownload> downloads;
    while (true) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            break;
        }
        if (result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read pending downloads: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        downloads.push_back({
            .item = {
                .drive_id = column_text(statement.get(), 0),
                .remote_id = column_text(statement.get(), 1),
                .parent_id = column_text(statement.get(), 2),
                .name = column_text(statement.get(), 3),
                .etag = column_text(statement.get(), 4),
                .remote_path = column_text(statement.get(), 5),
                .local_path = column_text(statement.get(), 6),
                .last_modified = column_text(statement.get(), 7),
                .size = sqlite3_column_int64(statement.get(), 8),
                .local_size = 0,
                .local_modified_ticks = 0,
                .directory = sqlite3_column_int(statement.get(), 9) != 0,
            },
            .temporary_path = column_text(statement.get(), 10),
            .content_fingerprint = column_text(statement.get(), 11),
        });
    }
    return downloads;
}

std::vector<BlockedItem> ItemDatabase::blocked_items(
    const std::string& drive_id
) const {
    std::vector<BlockedItem> result;
    for (const auto& [key, item] : blocked_items_) {
        static_cast<void>(key);
        if (item.drive_id == drive_id) {
            result.push_back(item);
        }
    }
    std::ranges::sort(result, {}, &BlockedItem::remote_path);
    return result;
}

bool ItemDatabase::reset(const std::string& drive_id) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (drive_id.empty()) {
        throw std::invalid_argument(
            "cannot reset synchronization state without a drive ID"
        );
    }

    const bool had_delta_link = delta_links_.contains(drive_id);
    Transaction transaction{database};
    Statement state_statement{
        database,
        "DELETE FROM drive_state WHERE drive_id = ?1;"
    };
    bind_text(database, state_statement.get(), 1, drive_id);
    if (sqlite3_step(state_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot reset drive delta link: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    transaction.commit();

    std::size_t retained_items = 0;
    for (const auto& [key, item] : items_) {
        static_cast<void>(key);
        retained_items += item.drive_id == drive_id ? 1U : 0U;
    }
    delta_links_.erase(drive_id);
    spdlog::debug(
        "Reset synchronization cursor for drive '{}': saved cursor {}, "
        "{} item snapshots retained",
        drive_id,
        had_delta_link ? "removed" : "not present",
        retained_items
    );
    return had_delta_link;
}

ClearedState ItemDatabase::clear(const std::string& drive_id) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (drive_id.empty()) {
        throw std::invalid_argument(
            "cannot clear synchronization state without a drive ID"
        );
    }

    ClearedState cleared{
        .delta_link = delta_links_.contains(drive_id),
    };
    Transaction transaction{database};
    Statement item_statement{
        database,
        "DELETE FROM item WHERE drive_id = ?1;"
    };
    bind_text(database, item_statement.get(), 1, drive_id);
    if (sqlite3_step(item_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear drive items: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    cleared.items = static_cast<std::size_t>(sqlite3_changes(database));

    Statement state_statement{
        database,
        "DELETE FROM drive_state WHERE drive_id = ?1;"
    };
    bind_text(database, state_statement.get(), 1, drive_id);
    if (sqlite3_step(state_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear drive delta link: " +
            std::string{sqlite3_errmsg(database)}
        );
    }

    Statement pending_statement{
        database,
        "DELETE FROM pending_download WHERE drive_id = ?1;"
    };
    bind_text(database, pending_statement.get(), 1, drive_id);
    if (sqlite3_step(pending_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear pending downloads: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    cleared.pending_downloads =
        static_cast<std::size_t>(sqlite3_changes(database));

    Statement blocked_statement{
        database,
        "DELETE FROM blocked_item WHERE drive_id = ?1;"
    };
    bind_text(database, blocked_statement.get(), 1, drive_id);
    if (sqlite3_step(blocked_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear blocked items: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    cleared.blocked_items =
        static_cast<std::size_t>(sqlite3_changes(database));
    transaction.commit();

    for (auto iterator = items_.begin(); iterator != items_.end();) {
        if (iterator->second.drive_id == drive_id) {
            iterator = items_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = blocked_items_.begin();
         iterator != blocked_items_.end();) {
        if (iterator->second.drive_id == drive_id) {
            iterator = blocked_items_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    delta_links_.erase(drive_id);
    spdlog::warn(
        "Cleared all synchronization state for drive '{}': {} item snapshots, "
        "{} pending downloads, {} blocked items, saved delta cursor {}",
        drive_id,
        cleared.items,
        cleared.pending_downloads,
        cleared.blocked_items,
        cleared.delta_link ? "removed" : "not present"
    );
    return cleared;
}

std::optional<std::string> ItemDatabase::delta_link(
    const std::string& drive_id
) const {
    const auto iterator = delta_links_.find(drive_id);
    return iterator == delta_links_.end() ?
               std::nullopt :
               std::optional<std::string>{iterator->second};
}

const ItemState* ItemDatabase::find(
    const std::string& drive_id,
    const std::string& remote_id
) const {
    const auto iterator = items_.find(item_key(drive_id, remote_id));
    return iterator == items_.end() ? nullptr : &iterator->second;
}

std::size_t ItemDatabase::size() const noexcept {
    return items_.size();
}

}  // namespace onedrive::storage
