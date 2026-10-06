#include "storage/sqlite.hpp"

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

}  // namespace

void execute(sqlite3* database, const char* sql) {
    char* raw_error_message = nullptr;
    const int result =
        sqlite3_exec(
            database,
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
            sqlite3_errmsg(database) :
            error_message.get();
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

void require_pragma_value(
    sqlite3* database,
    const char* sql,
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
