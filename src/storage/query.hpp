#pragma once

#include "storage/sqlite.hpp"

#include <sqlite3.h>

#include <cstddef>
#include <stdexcept>
#include <string>

namespace onedrive::storage::item_database_detail {

inline std::size_t query_count(
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

}  // namespace onedrive::storage::item_database_detail
