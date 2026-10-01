#include "onedrive/storage/item_database.hpp"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <stdexcept>
#include <string>
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

std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value == nullptr ? std::string{} :
                              std::string{reinterpret_cast<const char*>(value)};
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
    execute(database, "PRAGMA user_version = 1;");
    execute(
        database,
        "CREATE TABLE IF NOT EXISTS item ("
        "remote_id TEXT PRIMARY KEY NOT NULL,"
        "etag TEXT NOT NULL,"
        "local_path TEXT NOT NULL"
        ");"
    );

    Statement query{
        database,
        "SELECT remote_id, etag, local_path FROM item ORDER BY remote_id;"
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
            .remote_id = column_text(query.get(), 0),
            .etag = column_text(query.get(), 1),
            .local_path = column_text(query.get(), 2),
        };
        items_.insert_or_assign(item.remote_id, std::move(item));
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
        "INSERT INTO item (remote_id, etag, local_path) VALUES (?1, ?2, ?3) "
        "ON CONFLICT(remote_id) DO UPDATE SET "
        "etag = excluded.etag, local_path = excluded.local_path;"
    };
    const std::string local_path = item.local_path.string();
    bind_text(database, statement.get(), 1, item.remote_id);
    bind_text(database, statement.get(), 2, item.etag);
    bind_text(database, statement.get(), 3, local_path);

    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "cannot update state database: " + std::string{sqlite3_errmsg(database)}
        );
    }
    items_.insert_or_assign(item.remote_id, std::move(item));
    spdlog::trace(
        "Updated synchronization state; {} items tracked",
        items_.size()
    );
}

const ItemState* ItemDatabase::find(const std::string& remote_id) const {
    const auto iterator = items_.find(remote_id);
    return iterator == items_.end() ? nullptr : &iterator->second;
}

std::size_t ItemDatabase::size() const noexcept {
    return items_.size();
}

}  // namespace onedrive::storage
