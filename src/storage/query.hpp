#pragma once

#include "storage/sqlite.hpp"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace onedrive::storage::item_database_detail {

inline std::size_t query_count(
    sqlite3* database,
    const char* sql,
    std::optional<std::string_view> value = std::nullopt
) {
    Statement statement{database, sql};
    if (value) {
        bind_text(database, statement.get(), 1, *value);
    }
    statement.step_row("cannot count synchronization state rows");
    const auto count = sqlite3_column_int64(statement.get(), 0);
    if (count < 0 || static_cast<std::uintmax_t>(count) >
                         std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("SQLite row count is not representable");
    }
    return static_cast<std::size_t>(count);
}

} // namespace onedrive::storage::item_database_detail
