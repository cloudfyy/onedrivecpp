#include "storage/sqlite.hpp"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

namespace onedrive::storage::item_database_detail {
namespace {

struct SqliteFreer {
    void operator()(char* value) const noexcept {
        sqlite3_free(value);
    }
};

using SqliteString = std::unique_ptr<char, SqliteFreer>;

void log_cleanup_failure(
    std::string_view operation,
    std::string_view detail
) noexcept {
    try {
        spdlog::error("{}: {}", operation, detail);
    } catch (...) {
        std::fprintf(
            stderr,
            "%.*s: %.*s\n",
            static_cast<int>(operation.size()),
            operation.data(),
            static_cast<int>(detail.size()),
            detail.data()
        );
    }
}

}  // namespace

void SqliteCloser::operator()(sqlite3* handle) const noexcept {
    const int result = sqlite3_close_v2(handle);
    if (result != SQLITE_OK) {
        log_cleanup_failure(
            "cannot close SQLite database",
            sqlite3_errstr(result)
        );
    }
}

void SqliteStatementFinalizer::operator()(sqlite3_stmt* statement) const
    noexcept {
    const int result = sqlite3_finalize(statement);
    if (result != SQLITE_OK) {
        log_cleanup_failure(
            "cannot finalize SQLite statement",
            sqlite3_errstr(result)
        );
    }
}

Transaction::~Transaction() {
    if (committed_) {
        return;
    }
    const int result = sqlite3_exec(
        database_.get(),
        "ROLLBACK;",
        nullptr,
        nullptr,
        nullptr
    );
    if (result != SQLITE_OK) {
        log_cleanup_failure(
            "cannot roll back SQLite transaction",
            sqlite3_errmsg(database_.get())
        );
    }
}

void execute_sql(Database database, Sql sql) {
    char* raw_error_message = nullptr;
    const int result =
        sqlite3_exec(
            database.get(),
            sql,
            nullptr,
            nullptr,
            &raw_error_message
        );
    const SqliteString error_message{raw_error_message};
    if (result == SQLITE_OK) {
        return;
    }

    const std::string message =
        error_message == nullptr ?
            sqlite3_errmsg(database.get()) :
            error_message.get();
    throw std::runtime_error("SQLite operation failed: " + message);
}

void bind_text(
    Database database,
    PreparedStatement statement,
    int index,
    std::string_view value
) {
    const int result = sqlite3_bind_text64(
        statement.get(),
        index,
        value.data(),
        static_cast<sqlite3_uint64>(value.size()),
        SQLITE_TRANSIENT,
        SQLITE_UTF8
    );
    if (result != SQLITE_OK) {
        throw std::runtime_error(
            "cannot bind SQLite value: " +
            std::string{sqlite3_errmsg(database.get())}
        );
    }
}

void bind_integer(
    Database database,
    PreparedStatement statement,
    int index,
    std::int64_t value
) {
    if (sqlite3_bind_int64(statement.get(), index, value) != SQLITE_OK) {
        throw std::runtime_error(
            "cannot bind SQLite integer: " +
            std::string{sqlite3_errmsg(database.get())}
        );
    }
}

void bind_blob(
    Database database,
    PreparedStatement statement,
    int index,
    const std::vector<std::uint8_t>& value
) {
    const int result =
        value.empty() ?
            sqlite3_bind_zeroblob64(statement.get(), index, 0) :
            sqlite3_bind_blob64(
                statement.get(),
                index,
                value.data(),
                static_cast<sqlite3_uint64>(value.size()),
                SQLITE_TRANSIENT
            );
    if (result != SQLITE_OK) {
        throw std::runtime_error(
            "cannot bind SQLite blob: " +
            std::string{sqlite3_errmsg(database.get())}
        );
    }
}

std::string column_text(PreparedStatement statement, int column) {
    const auto* value = sqlite3_column_text(statement.get(), column);
    return value == nullptr ? std::string{} :
                              std::string{reinterpret_cast<const char*>(value)};
}

void require_pragma_value(
    Database database,
    Sql sql,
    std::string_view expected,
    std::string_view description
) {
    Statement statement{database, sql};
    if (sqlite3_step(statement.get()) != SQLITE_ROW ||
        column_text(statement.get(), 0) != expected ||
        sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "SQLite did not enable " + std::string{description}
        );
    }
}

}  // namespace onedrive::storage::item_database_detail
