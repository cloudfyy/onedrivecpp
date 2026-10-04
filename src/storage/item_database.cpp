#include "onedrive/storage/item_database.hpp"

#include <sqlite3.h>
#include <spdlog/spdlog.h>
#include <gsl/pointers>

#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <utility>

namespace onedrive::storage {
namespace {

class Statement {
public:
    Statement(gsl::not_null<sqlite3*> database, const char* sql)
        : database_{database} {
        const int result = sqlite3_prepare_v2(
            database_.get(),
            sql,
            -1,
            &statement_,
            nullptr
        );
        if (result != SQLITE_OK) {
            throw std::runtime_error(
                "cannot prepare SQLite statement: " +
                std::string{sqlite3_errmsg(database_.get())}
            );
        }
    }

    ~Statement() {
        sqlite3_finalize(statement_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&&) = delete;
    Statement& operator=(Statement&&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept {
        return statement_;
    }

private:
    gsl::not_null<sqlite3*> database_;
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

std::size_t query_count(
    sqlite3* database,
    const char* sql,
    const std::string* value = nullptr
) {
    Statement statement{database, sql};
    if (value != nullptr) {
        bind_text(database, statement.get(), 1, *value);
    }
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw std::runtime_error(
            "cannot count synchronization state rows: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    const auto count = sqlite3_column_int64(statement.get(), 0);
    if (count < 0) {
        throw std::runtime_error("SQLite returned a negative row count");
    }
    return static_cast<std::size_t>(count);
}

class Transaction {
public:
    explicit Transaction(gsl::not_null<sqlite3*> database)
        : database_{database} {
        execute(database_.get(), "BEGIN IMMEDIATE;");
    }

    ~Transaction() {
        if (!committed_) {
            sqlite3_exec(
                database_.get(),
                "ROLLBACK;",
                nullptr,
                nullptr,
                nullptr
            );
        }
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    void commit() {
        execute(database_.get(), "COMMIT;");
        committed_ = true;
    }

private:
    gsl::not_null<sqlite3*> database_;
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
        "delta_link TEXT NOT NULL,"
        "sync_filter_fingerprint TEXT NOT NULL DEFAULT ''"
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
        "backup_path TEXT NOT NULL DEFAULT '',"
        "backup_fingerprint TEXT NOT NULL DEFAULT '',"
        "PRIMARY KEY (drive_id, remote_id)"
        ");"
    );
    create_blocked_item_schema(database);
    create_identity_schema(database);
    create_partial_download_schema(database);
    create_pending_upload_schema(database);
    create_pending_move_schema(database);
    execute(database, "PRAGMA user_version = 15;");
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
        create_partial_download_schema(database);
        add_sync_filter_fingerprint(database);
        add_pending_download_backup(database);
        create_pending_upload_schema(database);
        create_pending_move_schema(database);
        execute(database, "PRAGMA user_version = 15;");
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
        create_partial_download_schema(database);
        add_sync_filter_fingerprint(database);
        add_pending_download_backup(database);
        create_pending_upload_schema(database);
        create_pending_move_schema(database);
        execute(database, "PRAGMA user_version = 15;");
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
        create_partial_download_schema(database);
        add_sync_filter_fingerprint(database);
        add_pending_download_backup(database);
        create_pending_upload_schema(database);
        create_pending_move_schema(database);
        execute(database, "PRAGMA user_version = 15;");
        transaction.commit();
        return;
    }
    if (version == 4) {
        Transaction transaction{database};
        create_blocked_item_schema(database);
        create_identity_schema(database);
        create_partial_download_schema(database);
        add_sync_filter_fingerprint(database);
        add_pending_download_backup(database);
        create_pending_upload_schema(database);
        create_pending_move_schema(database);
        execute(database, "PRAGMA user_version = 15;");
        transaction.commit();
        return;
    }
    if (version == 5) {
        Transaction transaction{database};
        add_blocked_item_hash_columns(database);
        add_blocked_item_deleted_column(database);
        create_identity_schema(database);
        create_partial_download_schema(database);
        add_sync_filter_fingerprint(database);
        add_pending_download_backup(database);
        create_pending_upload_schema(database);
        create_pending_move_schema(database);
        execute(database, "PRAGMA user_version = 15;");
        transaction.commit();
        return;
    }
    if (version == 6) {
        Transaction transaction{database};
        add_blocked_item_hash_columns(database);
        add_blocked_item_deleted_column(database);
        create_identity_schema(database);
        create_partial_download_schema(database);
        add_sync_filter_fingerprint(database);
        add_pending_download_backup(database);
        create_pending_upload_schema(database);
        create_pending_move_schema(database);
        execute(database, "PRAGMA user_version = 15;");
        transaction.commit();
        return;
    }
    if (version == 7) {
        Transaction transaction{database};
        add_blocked_item_hash_columns(database);
        add_blocked_item_deleted_column(database);
        create_partial_download_schema(database);
        add_sync_filter_fingerprint(database);
        add_pending_download_backup(database);
        create_pending_upload_schema(database);
        create_pending_move_schema(database);
        execute(database, "PRAGMA user_version = 15;");
        transaction.commit();
        return;
    }
    if (version == 8) {
        Transaction transaction{database};
        add_blocked_item_hash_columns(database);
        add_blocked_item_deleted_column(database);
        add_sync_filter_fingerprint(database);
        add_pending_download_backup(database);
        create_pending_upload_schema(database);
        create_pending_move_schema(database);
        execute(database, "PRAGMA user_version = 15;");
        transaction.commit();
        return;
    }
    if (version == 9) {
        Transaction transaction{database};
        add_sync_filter_fingerprint(database);
        add_pending_download_backup(database);
        add_blocked_item_deleted_column(database);
        create_pending_upload_schema(database);
        create_pending_move_schema(database);
        execute(database, "PRAGMA user_version = 15;");
        transaction.commit();
        return;
    }
    if (version == 10) {
        Transaction transaction{database};
        add_pending_download_backup(database);
        add_blocked_item_deleted_column(database);
        create_pending_upload_schema(database);
        create_pending_move_schema(database);
        execute(database, "PRAGMA user_version = 15;");
        transaction.commit();
        return;
    }
    if (version == 12) {
        Transaction transaction{database};
        create_pending_upload_schema(database);
        create_pending_move_schema(database);
        execute(database, "PRAGMA user_version = 15;");
        transaction.commit();
        return;
    }
    if (version == 11) {
        Transaction transaction{database};
        add_blocked_item_deleted_column(database);
        create_pending_upload_schema(database);
        create_pending_move_schema(database);
        execute(database, "PRAGMA user_version = 15;");
        transaction.commit();
        return;
    }
    if (version == 13) {
        Transaction transaction{database};
        create_pending_move_schema(database);
        execute(database, "PRAGMA user_version = 15;");
        transaction.commit();
        return;
    }
    if (version == 14) {
        Transaction transaction{database};
        add_pending_move_staging(database);
        execute(database, "PRAGMA user_version = 15;");
        transaction.commit();
        return;
    }
    if (version != 15) {
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

    Impl()
        : worker{[this] {
              run();
          }} {}

    ~Impl() {
        {
            std::lock_guard lock{mutex};
            stopping = true;
        }
        condition.notify_one();
        worker.join();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    template <typename Function>
    auto invoke(Function&& function) {
        using Result = std::invoke_result_t<Function>;

        std::promise<Result> promise;
        auto future = promise.get_future();
        {
            std::lock_guard lock{mutex};
            if (stopping) {
                throw std::runtime_error("state database is shutting down");
            }
            commands.emplace_back(
                [function = std::forward<Function>(function),
                 promise = std::move(promise)]() mutable {
                    try {
                        if constexpr (std::is_void_v<Result>) {
                            std::invoke(std::move(function));
                            promise.set_value();
                        } else {
                            promise.set_value(
                                std::invoke(std::move(function))
                            );
                        }
                    } catch (...) {
                        promise.set_exception(std::current_exception());
                    }
                }
            );
        }
        condition.notify_one();
        return future.get();
    }

    void run() {
        while (true) {
            std::move_only_function<void()> command;
            {
                std::unique_lock lock{mutex};
                condition.wait(lock, [this] {
                    return stopping || !commands.empty();
                });
                if (stopping && commands.empty()) {
                    return;
                }
                command = std::move(commands.front());
                commands.pop_front();
            }
            command();
        }
    }

    std::unique_ptr<sqlite3, DatabaseCloser> database;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::move_only_function<void()>> commands;
    bool stopping{false};
    std::jthread worker;
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
    impl_->invoke([this] {
        open_on_worker();
    });
}

void ItemDatabase::open_on_worker() {
    spdlog::debug("Opening synchronization state database");
    if (identity_.user_id.empty() || identity_.user_display_name.empty() ||
        identity_.configured_drive_id.empty() || identity_.drive_id.empty() ||
        identity_.drive_name.empty()) {
        throw std::invalid_argument(
            "state database requires a complete account and drive identity"
        );
    }
    std::filesystem::create_directories(state_directory_);
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

    const auto item_count =
        query_count(database, "SELECT COUNT(*) FROM item;");
    const auto blocked_count =
        query_count(database, "SELECT COUNT(*) FROM blocked_item;");
    spdlog::info(
        "Synchronization state database ready with {} tracked items and {} "
        "blocked items",
        item_count,
        blocked_count
    );
}

void ItemDatabase::upsert(ItemState item) {
    impl_->invoke([this, item = std::move(item)] {
        upsert_on_worker(item);
    });
}

void ItemDatabase::upsert_on_worker(const ItemState& item) {
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
    spdlog::trace(
        "Updated synchronization state for drive '{}', item '{}'",
        item.drive_id,
        item.remote_id
    );
}

void ItemDatabase::apply_delta(ItemDelta delta) {
    impl_->invoke([this, delta = std::move(delta)]() mutable {
        apply_delta_on_worker(std::move(delta));
    });
}

void ItemDatabase::apply_delta_on_worker(ItemDelta delta) {
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

    Statement complete_move_statement{
        database,
        "DELETE FROM pending_move WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    for (const auto& item : delta.upserts) {
        bind_text(
            database,
            complete_move_statement.get(),
            1,
            delta.drive_id
        );
        bind_text(
            database,
            complete_move_statement.get(),
            2,
            item.remote_id
        );
        if (sqlite3_step(complete_move_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot complete pending move journal: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        sqlite3_reset(complete_move_statement.get());
        sqlite3_clear_bindings(complete_move_statement.get());
    }
    for (const auto& remote_id : delta.removals) {
        bind_text(
            database,
            complete_move_statement.get(),
            1,
            delta.drive_id
        );
        bind_text(
            database,
            complete_move_statement.get(),
            2,
            remote_id
        );
        if (sqlite3_step(complete_move_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(
                "cannot discard removed pending move journal: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        sqlite3_reset(complete_move_statement.get());
        sqlite3_clear_bindings(complete_move_statement.get());
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
        "last_modified, size, directory, deleted, reason_code, reason_message, "
        "content_hash_algorithm, content_hash_value"
        ") VALUES ("
        "?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14"
        ") "
        "ON CONFLICT(drive_id, remote_id) DO UPDATE SET "
        "parent_id = excluded.parent_id, name = excluded.name, "
        "etag = excluded.etag, remote_path = excluded.remote_path, "
        "last_modified = excluded.last_modified, size = excluded.size, "
        "directory = excluded.directory, deleted = excluded.deleted, "
        "reason_code = excluded.reason_code, "
        "reason_message = excluded.reason_message, "
        "content_hash_algorithm = excluded.content_hash_algorithm, "
        "content_hash_value = excluded.content_hash_value, "
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
        bind_integer(
            database,
            upsert_blocked_statement.get(),
            10,
            item.deleted ? 1 : 0
        );
        bind_text(
            database,
            upsert_blocked_statement.get(),
            11,
            item.reason_code
        );
        bind_text(
            database,
            upsert_blocked_statement.get(),
            12,
            item.reason_message
        );
        std::string hash_algorithm;
        std::string hash_value;
        if (item.content_hash.has_value()) {
            hash_algorithm =
                item.content_hash->algorithm == FileHashAlgorithm::sha256 ?
                    "sha256" :
                    "quick_xor";
            hash_value = item.content_hash->value;
            if (hash_value.empty()) {
                throw std::invalid_argument(
                    "blocked item content hash cannot be empty"
                );
            }
        }
        bind_text(
            database,
            upsert_blocked_statement.get(),
            13,
            hash_algorithm
        );
        bind_text(database, upsert_blocked_statement.get(), 14, hash_value);
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
        "INSERT INTO drive_state ("
        "drive_id, delta_link, sync_filter_fingerprint"
        ") VALUES (?1, ?2, ?3) "
        "ON CONFLICT(drive_id) DO UPDATE SET "
        "delta_link = excluded.delta_link, "
        "sync_filter_fingerprint = excluded.sync_filter_fingerprint;"
    };
    bind_text(database, state_statement.get(), 1, delta.drive_id);
    bind_text(database, state_statement.get(), 2, delta.delta_link);
    bind_text(
        database,
        state_statement.get(),
        3,
        delta.sync_filter_fingerprint
    );
    if (sqlite3_step(state_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot update delta link: " + std::string{sqlite3_errmsg(database)}
        );
    }
    transaction.commit();

    spdlog::debug(
        "Committed remote delta for drive '{}': {} upserts, {} removals, {} "
        "blocked, inventory {}, delta cursor advanced, {} total items tracked",
        delta.drive_id,
        delta.upserts.size(),
        delta.removals.size(),
        delta.blocked_upserts.size(),
        delta.replace_drive_items ? "replaced" : "updated",
        query_count(database, "SELECT COUNT(*) FROM item;")
    );
}

void ItemDatabase::save_pending_download(PendingDownload download) {
    impl_->invoke([this, download = std::move(download)] {
        save_pending_download_on_worker(download);
    });
}

void ItemDatabase::save_pending_download_on_worker(
    const PendingDownload& download
) {
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
    if (download.backup_path.empty() !=
        download.backup_fingerprint.empty()) {
        throw std::invalid_argument(
            "pending download safeBackup path and fingerprint must be "
            "provided together"
        );
    }

    Statement statement{
        database,
        "INSERT INTO pending_download ("
        "drive_id, remote_id, parent_id, name, etag, remote_path, local_path, "
        "last_modified, size, directory, temporary_path, content_fingerprint"
        ", backup_path, backup_fingerprint"
        ") VALUES ("
        "?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14"
        ") "
        "ON CONFLICT(drive_id, remote_id) DO UPDATE SET "
        "parent_id = excluded.parent_id, name = excluded.name, "
        "etag = excluded.etag, remote_path = excluded.remote_path, "
        "local_path = excluded.local_path, "
        "last_modified = excluded.last_modified, size = excluded.size, "
        "directory = excluded.directory, temporary_path = excluded.temporary_path, "
        "content_fingerprint = excluded.content_fingerprint, "
        "backup_path = excluded.backup_path, "
        "backup_fingerprint = excluded.backup_fingerprint;"
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
    bind_text(database, statement.get(), 13, download.backup_path.string());
    bind_text(database, statement.get(), 14, download.backup_fingerprint);
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
    impl_->invoke([this, drive_id, remote_id] {
        remove_pending_download_on_worker(drive_id, remote_id);
    });
}

void ItemDatabase::remove_pending_download_on_worker(
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
    return impl_->invoke([this, drive_id] {
        return pending_downloads_on_worker(drive_id);
    });
}

std::vector<PendingDownload> ItemDatabase::pending_downloads_on_worker(
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
        "content_fingerprint, backup_path, backup_fingerprint "
        "FROM pending_download WHERE drive_id = ?1 "
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
            .backup_path = column_text(statement.get(), 12),
            .backup_fingerprint = column_text(statement.get(), 13),
        });
    }
    return downloads;
}

void ItemDatabase::save_partial_download(PartialDownload download) {
    impl_->invoke([this, download = std::move(download)] {
        save_partial_download_on_worker(download);
    });
}

void ItemDatabase::save_partial_download_on_worker(
    const PartialDownload& download
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (download.item.drive_id.empty() || download.item.remote_id.empty() ||
        download.temporary_path.empty() || download.item.size < 0 ||
        download.completed_bytes >
            static_cast<std::uint64_t>(download.item.size) ||
        download.completed_bytes >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()
            )) {
        throw std::invalid_argument(
            "partial download requires valid item, path, size, and offset"
        );
    }

    Statement statement{
        database,
        "INSERT INTO partial_download ("
        "drive_id, remote_id, parent_id, name, etag, remote_path, local_path, "
        "last_modified, size, directory, temporary_path, completed_bytes"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12) "
        "ON CONFLICT(drive_id, remote_id) DO UPDATE SET "
        "parent_id = excluded.parent_id, name = excluded.name, "
        "etag = excluded.etag, remote_path = excluded.remote_path, "
        "local_path = excluded.local_path, "
        "last_modified = excluded.last_modified, size = excluded.size, "
        "directory = excluded.directory, "
        "temporary_path = excluded.temporary_path, "
        "completed_bytes = excluded.completed_bytes, "
        "updated_at = unixepoch();"
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
    bind_integer(
        database,
        statement.get(),
        12,
        static_cast<std::int64_t>(download.completed_bytes)
    );
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot save partial download: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
}

void ItemDatabase::remove_partial_download(
    const std::string& drive_id,
    const std::string& remote_id
) {
    impl_->invoke([this, drive_id, remote_id] {
        remove_partial_download_on_worker(drive_id, remote_id);
    });
}

void ItemDatabase::remove_partial_download_on_worker(
    const std::string& drive_id,
    const std::string& remote_id
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "DELETE FROM partial_download WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    bind_text(database, statement.get(), 2, remote_id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot remove partial download: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
}

std::optional<PartialDownload> ItemDatabase::partial_download(
    const std::string& drive_id,
    const std::string& remote_id
) const {
    return impl_->invoke([this, drive_id, remote_id] {
        return partial_download_on_worker(drive_id, remote_id);
    });
}

std::optional<PartialDownload> ItemDatabase::partial_download_on_worker(
    const std::string& drive_id,
    const std::string& remote_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_id, parent_id, name, etag, remote_path, "
        "local_path, last_modified, size, directory, temporary_path, "
        "completed_bytes FROM partial_download "
        "WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    bind_text(database, statement.get(), 2, remote_id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return std::nullopt;
    }
    if (result != SQLITE_ROW) {
        throw std::runtime_error(
            "cannot read partial download: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    const auto completed = sqlite3_column_int64(statement.get(), 11);
    if (completed < 0) {
        throw std::runtime_error(
            "partial download contains a negative completed byte count"
        );
    }
    return PartialDownload{
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
        .completed_bytes = static_cast<std::uint64_t>(completed),
    };
}

void ItemDatabase::save_pending_upload(PendingUpload upload) {
    impl_->invoke([this, upload = std::move(upload)] {
        save_pending_upload_on_worker(upload);
    });
}

void ItemDatabase::save_pending_upload_on_worker(
    const PendingUpload& upload
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (upload.drive_id.empty() || upload.remote_path.empty() ||
        upload.local_path.empty() || upload.snapshot_path.empty() ||
        upload.content_fingerprint.empty() || upload.local_size < 0 ||
        upload.remote_id.has_value() != !upload.expected_etag.empty()) {
        throw std::invalid_argument("pending upload contains invalid metadata");
    }
    Statement statement{
        database,
        "INSERT INTO pending_upload ("
        "drive_id, remote_path, local_path, snapshot_path, "
        "content_fingerprint, local_size, local_modified_ticks, remote_id, "
        "expected_etag"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9) "
        "ON CONFLICT(drive_id, remote_path) DO UPDATE SET "
        "local_path = excluded.local_path, "
        "snapshot_path = excluded.snapshot_path, "
        "content_fingerprint = excluded.content_fingerprint, "
        "local_size = excluded.local_size, "
        "local_modified_ticks = excluded.local_modified_ticks, "
        "remote_id = excluded.remote_id, "
        "expected_etag = excluded.expected_etag;"
    };
    bind_text(database, statement.get(), 1, upload.drive_id);
    bind_text(database, statement.get(), 2, upload.remote_path);
    bind_text(database, statement.get(), 3, upload.local_path.string());
    bind_text(database, statement.get(), 4, upload.snapshot_path.string());
    bind_text(database, statement.get(), 5, upload.content_fingerprint);
    bind_integer(database, statement.get(), 6, upload.local_size);
    bind_integer(database, statement.get(), 7, upload.local_modified_ticks);
    bind_text(
        database,
        statement.get(),
        8,
        upload.remote_id.value_or("")
    );
    bind_text(database, statement.get(), 9, upload.expected_etag);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot persist pending upload: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
}

std::vector<PendingUpload> ItemDatabase::pending_uploads(
    const std::string& drive_id
) const {
    return impl_->invoke([this, drive_id] {
        return pending_uploads_on_worker(drive_id);
    });
}

std::vector<PendingUpload> ItemDatabase::pending_uploads_on_worker(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_path, local_path, snapshot_path, "
        "content_fingerprint, local_size, local_modified_ticks, remote_id, "
        "expected_etag FROM pending_upload WHERE drive_id = ?1 "
        "ORDER BY remote_path;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    std::vector<PendingUpload> uploads;
    while (true) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            break;
        }
        if (result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read pending uploads: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        auto remote_id = column_text(statement.get(), 7);
        uploads.push_back({
            .drive_id = column_text(statement.get(), 0),
            .remote_path = column_text(statement.get(), 1),
            .local_path = column_text(statement.get(), 2),
            .snapshot_path = column_text(statement.get(), 3),
            .content_fingerprint = column_text(statement.get(), 4),
            .local_size = sqlite3_column_int64(statement.get(), 5),
            .local_modified_ticks = sqlite3_column_int64(statement.get(), 6),
            .remote_id = remote_id.empty() ?
                std::nullopt :
                std::optional{std::move(remote_id)},
            .expected_etag = column_text(statement.get(), 8),
        });
    }
    return uploads;
}

void ItemDatabase::commit_upload(
    const PendingUpload& upload,
    ItemState item
) {
    impl_->invoke([this, upload, item = std::move(item)] {
        commit_upload_on_worker(upload, item);
    });
}

void ItemDatabase::commit_upload_on_worker(
    const PendingUpload& upload,
    const ItemState& item
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (upload.drive_id != item.drive_id ||
        upload.remote_path != item.remote_path) {
        throw std::invalid_argument(
            "pending upload and item state do not identify the same item"
        );
    }
    Transaction transaction{database};
    upsert_on_worker(item);
    Statement statement{
        database,
        "DELETE FROM pending_upload WHERE drive_id = ?1 AND remote_path = ?2;"
    };
    bind_text(database, statement.get(), 1, upload.drive_id);
    bind_text(database, statement.get(), 2, upload.remote_path);
    if (sqlite3_step(statement.get()) != SQLITE_DONE ||
        sqlite3_changes(database) != 1) {
        throw std::runtime_error(
            "cannot complete pending upload journal: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    transaction.commit();
}

void ItemDatabase::save_pending_move(PendingMove move) {
    impl_->invoke([this, move = std::move(move)] {
        save_pending_move_on_worker(move);
    });
}

void ItemDatabase::save_pending_move_on_worker(
    const PendingMove& move
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (move.drive_id.empty() || move.remote_id.empty() ||
        move.source_path.empty() || move.destination_path.empty() ||
        move.source_path == move.destination_path ||
        (!move.staging_path.empty() &&
         (move.staging_path == move.source_path ||
          move.staging_path == move.destination_path)) ||
        move.source_device >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()
            ) ||
        move.source_inode >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()
            )) {
        throw std::invalid_argument("pending move contains invalid metadata");
    }
    Statement statement{
        database,
        "INSERT INTO pending_move ("
        "drive_id, remote_id, source_path, destination_path, staging_path, "
        "source_device, source_inode, directory"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
        "ON CONFLICT(drive_id, remote_id) DO UPDATE SET "
        "source_path = excluded.source_path, "
        "destination_path = excluded.destination_path, "
        "staging_path = excluded.staging_path, "
        "source_device = excluded.source_device, "
        "source_inode = excluded.source_inode, "
        "directory = excluded.directory;"
    };
    bind_text(database, statement.get(), 1, move.drive_id);
    bind_text(database, statement.get(), 2, move.remote_id);
    bind_text(database, statement.get(), 3, move.source_path.string());
    bind_text(
        database,
        statement.get(),
        4,
        move.destination_path.string()
    );
    bind_text(database, statement.get(), 5, move.staging_path.string());
    bind_integer(
        database,
        statement.get(),
        6,
        static_cast<std::int64_t>(move.source_device)
    );
    bind_integer(
        database,
        statement.get(),
        7,
        static_cast<std::int64_t>(move.source_inode)
    );
    bind_integer(database, statement.get(), 8, move.directory ? 1 : 0);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot persist pending move: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
}

void ItemDatabase::remove_pending_move(
    const std::string& drive_id,
    const std::string& remote_id
) {
    impl_->invoke([this, drive_id, remote_id] {
        remove_pending_move_on_worker(drive_id, remote_id);
    });
}

void ItemDatabase::remove_pending_move_on_worker(
    const std::string& drive_id,
    const std::string& remote_id
) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "DELETE FROM pending_move WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    bind_text(database, statement.get(), 2, remote_id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot remove pending move: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
}

std::vector<PendingMove> ItemDatabase::pending_moves(
    const std::string& drive_id
) const {
    return impl_->invoke([this, drive_id] {
        return pending_moves_on_worker(drive_id);
    });
}

std::vector<PendingMove> ItemDatabase::pending_moves_on_worker(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_id, source_path, destination_path, "
        "staging_path, source_device, source_inode, directory "
        "FROM pending_move "
        "WHERE drive_id = ?1 ORDER BY remote_id;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    std::vector<PendingMove> moves;
    while (true) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            break;
        }
        if (result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read pending moves: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        const auto device = sqlite3_column_int64(statement.get(), 5);
        const auto inode = sqlite3_column_int64(statement.get(), 6);
        if (device < 0 || inode < 0) {
            throw std::runtime_error(
                "pending move contains invalid filesystem identity"
            );
        }
        moves.push_back({
            .drive_id = column_text(statement.get(), 0),
            .remote_id = column_text(statement.get(), 1),
            .source_path = column_text(statement.get(), 2),
            .destination_path = column_text(statement.get(), 3),
            .staging_path = column_text(statement.get(), 4),
            .source_device = static_cast<std::uint64_t>(device),
            .source_inode = static_cast<std::uint64_t>(inode),
            .directory = sqlite3_column_int(statement.get(), 7) != 0,
        });
    }
    return moves;
}

std::vector<BlockedItem> ItemDatabase::blocked_items(
    const std::string& drive_id
) const {
    return impl_->invoke([this, drive_id] {
        return blocked_items_on_worker(drive_id);
    });
}

std::vector<BlockedItem> ItemDatabase::blocked_items_on_worker(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_id, parent_id, name, etag, remote_path, "
        "last_modified, size, directory, deleted, reason_code, reason_message, "
        "attempt_count, content_hash_algorithm, content_hash_value "
        "FROM blocked_item WHERE drive_id = ?1 "
        "ORDER BY remote_path;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    std::vector<BlockedItem> result;
    while (true) {
        const int step_result = sqlite3_step(statement.get());
        if (step_result == SQLITE_DONE) {
            break;
        }
        if (step_result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read blocked synchronization items: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        const std::string hash_algorithm = column_text(statement.get(), 13);
        const std::string hash_value = column_text(statement.get(), 14);
        std::optional<FileHash> content_hash;
        if (!hash_algorithm.empty() || !hash_value.empty()) {
            if (hash_value.empty() ||
                (hash_algorithm != "sha256" &&
                 hash_algorithm != "quick_xor")) {
                throw std::runtime_error(
                    "blocked item contains invalid content hash metadata"
                );
            }
            content_hash = FileHash{
                .algorithm = hash_algorithm == "sha256" ?
                    FileHashAlgorithm::sha256 :
                    FileHashAlgorithm::quick_xor,
                .value = hash_value,
            };
        }
        result.push_back({
            .drive_id = column_text(statement.get(), 0),
            .remote_id = column_text(statement.get(), 1),
            .parent_id = column_text(statement.get(), 2),
            .name = column_text(statement.get(), 3),
            .etag = column_text(statement.get(), 4),
            .remote_path = column_text(statement.get(), 5),
            .last_modified = column_text(statement.get(), 6),
            .size = sqlite3_column_int64(statement.get(), 7),
            .directory = sqlite3_column_int(statement.get(), 8) != 0,
            .deleted = sqlite3_column_int(statement.get(), 9) != 0,
            .reason_code = column_text(statement.get(), 10),
            .reason_message = column_text(statement.get(), 11),
            .attempt_count = static_cast<std::uint64_t>(
                sqlite3_column_int64(statement.get(), 12)
            ),
            .content_hash = std::move(content_hash),
        });
    }
    return result;
}

bool ItemDatabase::reset(const std::string& drive_id) {
    return impl_->invoke([this, drive_id] {
        return reset_on_worker(drive_id);
    });
}

bool ItemDatabase::reset_on_worker(const std::string& drive_id) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (drive_id.empty()) {
        throw std::invalid_argument(
            "cannot reset synchronization state without a drive ID"
        );
    }

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
    const bool had_delta_link = sqlite3_changes(database) != 0;
    transaction.commit();

    const auto retained_items = query_count(
        database,
        "SELECT COUNT(*) FROM item WHERE drive_id = ?1;",
        &drive_id
    );
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
    return impl_->invoke([this, drive_id] {
        return clear_on_worker(drive_id);
    });
}

ClearedState ItemDatabase::clear_on_worker(const std::string& drive_id) {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    if (drive_id.empty()) {
        throw std::invalid_argument(
            "cannot clear synchronization state without a drive ID"
        );
    }

    ClearedState cleared;
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
    cleared.delta_link = sqlite3_changes(database) != 0;

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

    Statement partial_statement{
        database,
        "DELETE FROM partial_download WHERE drive_id = ?1;"
    };
    bind_text(database, partial_statement.get(), 1, drive_id);
    if (sqlite3_step(partial_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear partial downloads: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    cleared.partial_downloads =
        static_cast<std::size_t>(sqlite3_changes(database));

    Statement upload_statement{
        database,
        "DELETE FROM pending_upload WHERE drive_id = ?1;"
    };
    bind_text(database, upload_statement.get(), 1, drive_id);
    if (sqlite3_step(upload_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear pending uploads: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    cleared.pending_uploads =
        static_cast<std::size_t>(sqlite3_changes(database));

    Statement move_statement{
        database,
        "DELETE FROM pending_move WHERE drive_id = ?1;"
    };
    bind_text(database, move_statement.get(), 1, drive_id);
    if (sqlite3_step(move_statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot clear pending moves: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    cleared.pending_moves =
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

    spdlog::warn(
        "Cleared all synchronization state for drive '{}': {} item snapshots, "
        "{} pending downloads, {} partial downloads, {} pending uploads, {} "
        "pending moves, {} blocked items, saved delta cursor {}",
        drive_id,
        cleared.items,
        cleared.pending_downloads,
        cleared.partial_downloads,
        cleared.pending_uploads,
        cleared.pending_moves,
        cleared.blocked_items,
        cleared.delta_link ? "removed" : "not present"
    );
    return cleared;
}

std::optional<std::string> ItemDatabase::delta_link(
    const std::string& drive_id
) const {
    return impl_->invoke([this, drive_id] {
        return delta_link_on_worker(drive_id);
    });
}

std::optional<std::string> ItemDatabase::delta_link_on_worker(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }

    Statement statement{
        database,
        "SELECT delta_link FROM drive_state WHERE drive_id = ?1;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return std::nullopt;
    }
    if (result != SQLITE_ROW) {
        throw std::runtime_error(
            "cannot read drive delta link: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    return column_text(statement.get(), 0);
}

std::optional<std::string> ItemDatabase::sync_filter_fingerprint(
    const std::string& drive_id
) const {
    return impl_->invoke([this, drive_id] {
        return sync_filter_fingerprint_on_worker(drive_id);
    });
}

std::optional<std::string>
ItemDatabase::sync_filter_fingerprint_on_worker(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT sync_filter_fingerprint FROM drive_state "
        "WHERE drive_id = ?1;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return std::nullopt;
    }
    if (result != SQLITE_ROW) {
        throw std::runtime_error(
            "cannot read sync filter fingerprint: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    return column_text(statement.get(), 0);
}

std::optional<ItemState> ItemDatabase::find(
    const std::string& drive_id,
    const std::string& remote_id
) const {
    return impl_->invoke([this, drive_id, remote_id] {
        return find_on_worker(drive_id, remote_id);
    });
}

std::optional<ItemState> ItemDatabase::find_on_worker(
    const std::string& drive_id,
    const std::string& remote_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_id, parent_id, name, etag, remote_path, "
        "local_path, last_modified, size, local_size, local_modified_ticks, "
        "directory FROM item WHERE drive_id = ?1 AND remote_id = ?2;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    bind_text(database, statement.get(), 2, remote_id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return std::nullopt;
    }
    if (result != SQLITE_ROW) {
        throw std::runtime_error(
            "cannot read synchronization item: " +
            std::string{sqlite3_errmsg(database)}
        );
    }
    return ItemState{
        .drive_id = column_text(statement.get(), 0),
        .remote_id = column_text(statement.get(), 1),
        .parent_id = column_text(statement.get(), 2),
        .name = column_text(statement.get(), 3),
        .etag = column_text(statement.get(), 4),
        .remote_path = column_text(statement.get(), 5),
        .local_path = column_text(statement.get(), 6),
        .last_modified = column_text(statement.get(), 7),
        .size = sqlite3_column_int64(statement.get(), 8),
        .local_size = sqlite3_column_int64(statement.get(), 9),
        .local_modified_ticks = sqlite3_column_int64(statement.get(), 10),
        .directory = sqlite3_column_int(statement.get(), 11) != 0,
    };
}

std::vector<ItemState> ItemDatabase::drive_items(
    const std::string& drive_id
) const {
    return impl_->invoke([this, drive_id] {
        return drive_items_on_worker(drive_id);
    });
}

std::vector<ItemState> ItemDatabase::drive_items_on_worker(
    const std::string& drive_id
) const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    Statement statement{
        database,
        "SELECT drive_id, remote_id, parent_id, name, etag, remote_path, "
        "local_path, last_modified, size, local_size, local_modified_ticks, "
        "directory FROM item WHERE drive_id = ?1 ORDER BY remote_path;"
    };
    bind_text(database, statement.get(), 1, drive_id);
    std::vector<ItemState> result;
    while (true) {
        const int step_result = sqlite3_step(statement.get());
        if (step_result == SQLITE_DONE) {
            break;
        }
        if (step_result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot read synchronization drive items: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        result.push_back({
            .drive_id = column_text(statement.get(), 0),
            .remote_id = column_text(statement.get(), 1),
            .parent_id = column_text(statement.get(), 2),
            .name = column_text(statement.get(), 3),
            .etag = column_text(statement.get(), 4),
            .remote_path = column_text(statement.get(), 5),
            .local_path = column_text(statement.get(), 6),
            .last_modified = column_text(statement.get(), 7),
            .size = sqlite3_column_int64(statement.get(), 8),
            .local_size = sqlite3_column_int64(statement.get(), 9),
            .local_modified_ticks =
                sqlite3_column_int64(statement.get(), 10),
            .directory = sqlite3_column_int(statement.get(), 11) != 0,
        });
    }
    return result;
}

std::size_t ItemDatabase::size() const {
    return impl_->invoke([this] {
        return size_on_worker();
    });
}

std::size_t ItemDatabase::size_on_worker() const {
    sqlite3* database = impl_->database.get();
    if (database == nullptr) {
        throw std::runtime_error("state database is not open");
    }
    return query_count(database, "SELECT COUNT(*) FROM item;");
}

}  // namespace onedrive::storage
