#include "storage/schema.hpp"
#include "storage/schema_internal.hpp"
#include "storage/sqlite.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace onedrive::storage::item_database_detail {

constexpr int maximum_database_pages = 8 * 1024 * 1024;

std::string quote_identifier(std::string_view identifier) {
    std::string quoted{"\""};
    for (const char character : identifier) {
        quoted.push_back(character);
        if (character == '"') {
            quoted.push_back('"');
        }
    }
    quoted.push_back('"');
    return quoted;
}

std::vector<std::string> user_tables(sqlite3* database) {
    Statement statement{
        database,
        "SELECT name FROM sqlite_schema "
        "WHERE type = 'table' AND name NOT LIKE 'sqlite_%' "
        "ORDER BY name;"
    };
    std::vector<std::string> tables;
    for (int result = sqlite3_step(statement.get());
         result != SQLITE_DONE;
         result = sqlite3_step(statement.get())) {
        if (result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot inspect state database tables: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        tables.push_back(column_text(statement.get(), 0));
    }
    return tables;
}

struct ColumnShape {
    std::string name;
    std::string type;
    int not_null;
    int primary_key_position;

    auto operator<=>(const ColumnShape&) const = default;
};

std::vector<ColumnShape> table_columns(
    sqlite3* database,
    const std::string& table
) {
    const auto sql = "PRAGMA table_info(" + quote_identifier(table) + ");";
    Statement statement{database, sql.c_str()};
    std::vector<ColumnShape> columns;
    for (int result = sqlite3_step(statement.get());
         result != SQLITE_DONE;
         result = sqlite3_step(statement.get())) {
        if (result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot inspect columns for state database table '" + table +
                "': " + sqlite3_errmsg(database)
            );
        }
        columns.push_back({
            .name = column_text(statement.get(), 1),
            .type = column_text(statement.get(), 2),
            .not_null = sqlite3_column_int(statement.get(), 3),
            .primary_key_position = sqlite3_column_int(statement.get(), 5),
        });
    }
    std::ranges::sort(columns, {}, &ColumnShape::name);
    return columns;
}

struct IndexShape {
    std::vector<std::string> columns;
    int unique;
    std::string origin;
    int partial;

    auto operator<=>(const IndexShape&) const = default;
};

std::vector<IndexShape> table_indexes(
    sqlite3* database,
    const std::string& table
) {
    const auto list_sql =
        "PRAGMA index_list(" + quote_identifier(table) + ");";
    Statement list{database, list_sql.c_str()};
    std::vector<IndexShape> indexes;
    for (int result = sqlite3_step(list.get());
         result != SQLITE_DONE;
         result = sqlite3_step(list.get())) {
        if (result != SQLITE_ROW) {
            throw std::runtime_error(
                "cannot inspect indexes for state database table '" + table +
                "': " + sqlite3_errmsg(database)
            );
        }
        const auto index_name = column_text(list.get(), 1);
        const auto info_sql =
            "PRAGMA index_info(" + quote_identifier(index_name) + ");";
        Statement info{database, info_sql.c_str()};
        IndexShape index{
            .columns = {},
            .unique = sqlite3_column_int(list.get(), 2),
            .origin = column_text(list.get(), 3),
            .partial = sqlite3_column_int(list.get(), 4),
        };
        for (int info_result = sqlite3_step(info.get());
             info_result != SQLITE_DONE;
             info_result = sqlite3_step(info.get())) {
            if (info_result != SQLITE_ROW) {
                throw std::runtime_error(
                    "cannot inspect state database index '" + index_name +
                    "': " + sqlite3_errmsg(database)
                );
            }
            index.columns.push_back(column_text(info.get(), 2));
        }
        indexes.push_back(std::move(index));
    }
    std::ranges::sort(indexes);
    return indexes;
}

void verify_database_integrity(sqlite3* database) {
    try {
        Statement statement{database, "PRAGMA quick_check;"};
        if (sqlite3_step(statement.get()) != SQLITE_ROW) {
            throw DatabaseCorruption(
                "SQLite quick_check could not inspect the state database: " +
                std::string{sqlite3_errmsg(database)}
            );
        }
        const auto result = column_text(statement.get(), 0);
        if (result != "ok") {
            throw DatabaseCorruption(
                "SQLite quick_check reported state database corruption: " +
                result
            );
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw DatabaseCorruption(
                "SQLite quick_check returned unexpected additional results"
            );
        }
    } catch (const DatabaseCorruption&) {
        throw;
    } catch (const std::exception& error) {
        const int code = sqlite3_errcode(database) & 0xff;
        if (code == SQLITE_CORRUPT || code == SQLITE_NOTADB) {
            throw DatabaseCorruption(error.what());
        }
        throw;
    }
}

std::string full_integrity_result(sqlite3* database) {
    Statement integrity{database, "PRAGMA integrity_check;"};
    std::string detail;
    while (true) {
        const int result = sqlite3_step(integrity.get());
        if (result == SQLITE_DONE) {
            break;
        }
        if (result != SQLITE_ROW) {
            return "SQLite integrity_check failed: " +
                   std::string{sqlite3_errmsg(database)};
        }
        const auto message = column_text(integrity.get(), 0);
        if (message != "ok") {
            if (!detail.empty()) {
                detail += "; ";
            }
            detail += message;
        }
    }

    Statement foreign_keys{database, "PRAGMA foreign_key_check;"};
    if (sqlite3_step(foreign_keys.get()) != SQLITE_DONE) {
        if (!detail.empty()) {
            detail += "; ";
        }
        detail += "foreign key constraint violation";
    }
    return detail.empty() ? "ok" : detail;
}

void configure_writable_database(sqlite3* database) {
    execute(database, "PRAGMA journal_mode = WAL;");
    execute(database, "PRAGMA synchronous = FULL;");
    execute(database, "PRAGMA secure_delete = FAST;");
    execute(database, "PRAGMA wal_autocheckpoint = 1000;");
    const auto maximum_pages =
        "PRAGMA max_page_count = " + std::to_string(maximum_database_pages) +
        ";";
    execute(database, maximum_pages.c_str());
    require_pragma_value(
        database, "PRAGMA journal_mode;", "wal", "WAL journal mode"
    );
    require_pragma_value(
        database, "PRAGMA synchronous;", "2", "full synchronous writes"
    );
    require_pragma_value(
        database, "PRAGMA secure_delete;", "2", "fast secure deletion"
    );
    require_pragma_value(
        database,
        "PRAGMA wal_autocheckpoint;",
        "1000",
        "automatic WAL checkpoints"
    );
    require_pragma_value(
        database,
        "PRAGMA max_page_count;",
        std::to_string(maximum_database_pages),
        "database page limit"
    );
}

std::filesystem::path
quarantine_corrupt_database(const std::filesystem::path& database_path) {
    const auto timestamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        )
            .count();
    std::filesystem::path quarantine_path{
        database_path.string() + ".corrupt-" + std::to_string(timestamp)
    };
    for (std::size_t suffix = 1; std::filesystem::exists(quarantine_path);
         ++suffix) {
        quarantine_path = database_path.string() + ".corrupt-" +
                          std::to_string(timestamp) + "-" +
                          std::to_string(suffix);
    }

    const std::array sidecars{
        std::pair{
            std::filesystem::path{database_path.string() + "-wal"},
            std::filesystem::path{quarantine_path.string() + "-wal"},
        },
        std::pair{
            std::filesystem::path{database_path.string() + "-shm"},
            std::filesystem::path{quarantine_path.string() + "-shm"},
        },
    };
    for (const auto& [source, destination] : sidecars) {
        if (std::filesystem::exists(source)) {
            std::filesystem::rename(source, destination);
        }
    }
    std::filesystem::rename(database_path, quarantine_path);
    spdlog::error(
        "Quarantined corrupt synchronization state database as '{}'",
        quarantine_path.string()
    );
    return quarantine_path;
}

void verify_current_schema(sqlite3* database) {
    sqlite3* reference_handle = nullptr;
    const int result = sqlite3_open(":memory:", &reference_handle);
    SqliteHandle reference{reference_handle};
    if (result != SQLITE_OK) {
        const std::string message =
            reference == nullptr ?
                "unknown SQLite error" :
                sqlite3_errmsg(reference.get());
        throw std::runtime_error(
            "cannot create reference state database schema: " + message
        );
    }
    ensure_current_schema(reference.get());

    const auto expected_tables = user_tables(reference.get());
    const auto actual_tables = user_tables(database);
    for (const auto& expected : expected_tables) {
        if (!std::ranges::contains(actual_tables, expected)) {
            throw std::runtime_error(
                "state database schema is missing table '" + expected + "'"
            );
        }
    }
    for (const auto& actual : actual_tables) {
        if (!std::ranges::contains(expected_tables, actual)) {
            throw std::runtime_error(
                "state database schema contains unexpected table '" + actual +
                "'"
            );
        }
    }

    for (const auto& table : expected_tables) {
        const auto expected_columns = table_columns(reference.get(), table);
        const auto actual_columns = table_columns(database, table);
        for (const auto& expected : expected_columns) {
            const auto actual = std::ranges::find(
                actual_columns,
                expected.name,
                &ColumnShape::name
            );
            if (actual == actual_columns.end()) {
                throw std::runtime_error(
                    "state database table '" + table +
                    "' is missing column '" + expected.name + "'"
                );
            }
            if (*actual != expected) {
                throw std::runtime_error(
                    "state database table '" + table + "' column '" +
                    expected.name + "' has an incompatible definition"
                );
            }
        }
        for (const auto& actual : actual_columns) {
            if (!std::ranges::contains(
                    expected_columns,
                    actual.name,
                    &ColumnShape::name
                )) {
                throw std::runtime_error(
                    "state database table '" + table +
                    "' contains unexpected column '" + actual.name + "'"
                );
            }
        }
        if (table_indexes(database, table) !=
            table_indexes(reference.get(), table)) {
            throw std::runtime_error(
                "state database table '" + table +
                "' has incompatible primary key or index definitions"
            );
        }
    }
}

}  // namespace onedrive::storage::item_database_detail
