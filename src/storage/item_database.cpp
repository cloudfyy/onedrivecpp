#include "onedrive/storage/item_database.hpp"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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
        "PRAGMA user_version = 3;"
    );
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
            "PRAGMA user_version = 3;"
        );
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
            "PRAGMA user_version = 3;"
        );
        transaction.commit();
        return;
    }
    if (version != 3) {
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

ItemDatabase::ItemDatabase(std::filesystem::path state_directory)
    : state_directory_{std::move(state_directory)}, impl_{std::make_unique<Impl>()} {}

ItemDatabase::~ItemDatabase() = default;

void ItemDatabase::open() {
    spdlog::debug("Opening synchronization state database");
    std::filesystem::create_directories(state_directory_);
    items_.clear();
    delta_links_.clear();
    impl_->database.reset();

    sqlite3* database = nullptr;
    const auto database_path = state_directory_ / "items.sqlite3";
    const int result = sqlite3_open_v2(
        database_path.string().c_str(),
        &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
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

    execute(database, "PRAGMA journal_mode = WAL;");
    migrate_schema(database);

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
        "Synchronization state database ready with {} tracked items",
        items_.size()
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

    for (auto& item : delta.upserts) {
        const std::string key = item_key(delta.drive_id, item.remote_id);
        items_.insert_or_assign(key, std::move(item));
    }
    for (const auto& remote_id : delta.removals) {
        items_.erase(item_key(delta.drive_id, remote_id));
    }
    delta_links_.insert_or_assign(delta.drive_id, std::move(delta.delta_link));
    spdlog::debug(
        "Committed remote delta for drive '{}': {} upserts, {} removals, "
        "delta cursor advanced, {} total items tracked",
        delta.drive_id,
        delta.upserts.size(),
        delta.removals.size(),
        items_.size()
    );
}

std::size_t ItemDatabase::reset(const std::string& drive_id) {
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
    Statement item_statement{
        database,
        "DELETE FROM item WHERE drive_id = ?1;"
    };
    bind_text(database, item_statement.get(), 1, drive_id);
    if (sqlite3_step(item_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot reset drive items: " + std::string{sqlite3_errmsg(database)}
        );
    }
    const auto removed = static_cast<std::size_t>(sqlite3_changes(database));

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

    for (auto iterator = items_.begin(); iterator != items_.end();) {
        if (iterator->second.drive_id == drive_id) {
            iterator = items_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    delta_links_.erase(drive_id);
    spdlog::debug(
        "Cleared synchronization state for drive '{}': {} items removed, "
        "saved delta cursor {}",
        drive_id,
        removed,
        had_delta_link ? "removed" : "not present"
    );
    return removed;
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
