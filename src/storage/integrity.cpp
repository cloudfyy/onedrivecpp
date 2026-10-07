#include "storage/schema.hpp"
#include "storage/schema_internal.hpp"
#include "storage/sqlite.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
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
    while (statement.next_row("cannot inspect state database tables")) {
        tables.push_back(column_text(statement.get(), 0));
    }
    return tables;
}

struct ColumnShape {
    std::string name;
    std::string type;
    int not_null;
    std::string default_value;
    int primary_key_position;

    auto operator<=>(const ColumnShape&) const = default;
};

bool column_is_compatible(
    const ColumnShape& actual, const ColumnShape& expected
) {
    return actual.name == expected.name && actual.type == expected.type &&
           actual.not_null == expected.not_null &&
           (expected.default_value.empty() ||
            actual.default_value == expected.default_value) &&
           actual.primary_key_position == expected.primary_key_position;
}

std::vector<ColumnShape>
table_columns(sqlite3* database, const std::string& table) {
    const auto sql = "PRAGMA table_info(" + quote_identifier(table) + ");";
    Statement statement{database, sql.c_str()};
    std::vector<ColumnShape> columns;
    const auto operation =
        "cannot inspect columns for state database table '" + table + "'";
    while (statement.next_row(operation)) {
        columns.push_back({
            .name = column_text(statement.get(), 1),
            .type = column_text(statement.get(), 2),
            .not_null = sqlite3_column_int(statement.get(), 3),
            .default_value = column_text(statement.get(), 4),
            .primary_key_position = sqlite3_column_int(statement.get(), 5),
        });
    }
    std::ranges::sort(columns, {}, &ColumnShape::name);
    return columns;
}

struct IndexColumnShape {
    std::string name;
    int expression;
    int descending;
    std::string collation;
    int key;

    auto operator<=>(const IndexColumnShape&) const = default;
};

struct IndexShape {
    std::string name;
    std::vector<IndexColumnShape> columns;
    int unique;
    std::string origin;
    int partial;

    auto operator<=>(const IndexShape&) const = default;
};

std::vector<IndexShape>
table_indexes(sqlite3* database, const std::string& table) {
    const auto list_sql = "PRAGMA index_list(" + quote_identifier(table) + ");";
    Statement list{database, list_sql.c_str()};
    std::vector<IndexShape> indexes;
    const auto list_operation =
        "cannot inspect indexes for state database table '" + table + "'";
    while (list.next_row(list_operation)) {
        const auto index_name = column_text(list.get(), 1);
        const auto info_sql =
            "PRAGMA index_xinfo(" + quote_identifier(index_name) + ");";
        Statement info{database, info_sql.c_str()};
        IndexShape index{
            .name = index_name,
            .columns = {},
            .unique = sqlite3_column_int(list.get(), 2),
            .origin = column_text(list.get(), 3),
            .partial = sqlite3_column_int(list.get(), 4),
        };
        const auto info_operation =
            "cannot inspect state database index '" + index_name + "'";
        while (info.next_row(info_operation)) {
            const int column_id = sqlite3_column_int(info.get(), 1);
            index.columns.push_back({
                .name = column_text(info.get(), 2),
                .expression = column_id == -2,
                .descending = sqlite3_column_int(info.get(), 3),
                .collation = column_text(info.get(), 4),
                .key = sqlite3_column_int(info.get(), 5),
            });
        }
        indexes.push_back(std::move(index));
    }
    std::ranges::sort(indexes);
    return indexes;
}

std::string table_definition(sqlite3* database, const std::string& table) {
    Statement statement{
        database,
        "SELECT sql FROM sqlite_schema WHERE type = 'table' AND name = ?1;"
    };
    bind_text(database, statement.get(), 1, table);
    statement.require_row(
        "cannot inspect definition for state database table '" + table + "'"
    );
    return column_text(statement.get(), 0);
}

bool is_identifier_character(char character) {
    const auto value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 || character == '_';
}

bool starts_with_keyword(
    std::string_view sql, std::size_t position, std::string_view keyword
) {
    if (position + keyword.size() > sql.size() ||
        (position > 0 && is_identifier_character(sql[position - 1])) ||
        (position + keyword.size() < sql.size() &&
         is_identifier_character(sql[position + keyword.size()]))) {
        return false;
    }
    for (std::size_t offset = 0; offset < keyword.size(); ++offset) {
        const auto character =
            static_cast<unsigned char>(sql[position + offset]);
        if (std::tolower(character) != keyword[offset]) {
            return false;
        }
    }
    return true;
}

std::size_t skip_quoted_sql(std::string_view sql, std::size_t position) {
    const char quote = sql[position];
    const char closing = quote == '[' ? ']' : quote;
    ++position;
    while (position < sql.size()) {
        if (sql[position] != closing) {
            ++position;
            continue;
        }
        if (closing != ']' && position + 1 < sql.size() &&
            sql[position + 1] == closing) {
            position += 2;
            continue;
        }
        return position + 1;
    }
    return sql.size();
}

std::string normalize_sql_expression(std::string_view expression) {
    std::string normalized;
    for (std::size_t position = 0; position < expression.size();) {
        const char character = expression[position];
        if (character == '\'' || character == '"' || character == '`' ||
            character == '[') {
            const auto end = skip_quoted_sql(expression, position);
            normalized.append(expression.substr(position, end - position));
            position = end;
            continue;
        }
        const auto value = static_cast<unsigned char>(character);
        if (std::isspace(value) == 0) {
            normalized.push_back(
                static_cast<char>(std::tolower(value))
            );
        }
        ++position;
    }
    return normalized;
}

std::vector<std::string>
table_check_constraints(sqlite3* database, const std::string& table) {
    const auto definition = table_definition(database, table);
    std::vector<std::string> constraints;
    for (std::size_t position = 0; position < definition.size();) {
        const char character = definition[position];
        if (character == '\'' || character == '"' || character == '`' ||
            character == '[') {
            position = skip_quoted_sql(definition, position);
            continue;
        }
        if (!starts_with_keyword(definition, position, "check")) {
            ++position;
            continue;
        }

        position += std::string_view{"check"}.size();
        while (position < definition.size() &&
               std::isspace(
                   static_cast<unsigned char>(definition[position])
               ) != 0) {
            ++position;
        }
        if (position == definition.size() || definition[position] != '(') {
            continue;
        }

        const auto expression_start = position;
        int depth = 0;
        while (position < definition.size()) {
            const char current = definition[position];
            if (current == '\'' || current == '"' || current == '`' ||
                current == '[') {
                position = skip_quoted_sql(definition, position);
                continue;
            }
            if (current == '(') {
                ++depth;
            } else if (current == ')' && --depth == 0) {
                ++position;
                constraints.push_back(normalize_sql_expression(
                    std::string_view{definition}.substr(
                        expression_start, position - expression_start
                    )
                ));
                break;
            }
            ++position;
        }
    }
    std::ranges::sort(constraints);
    return constraints;
}

struct SchemaObjectShape {
    std::string type;
    std::string name;
    std::string table;

    auto operator<=>(const SchemaObjectShape&) const = default;
};

std::vector<SchemaObjectShape> user_schema_objects(sqlite3* database) {
    Statement statement{
        database,
        "SELECT type, name, tbl_name FROM sqlite_schema "
        "WHERE type IN ('view', 'trigger') AND name NOT LIKE 'sqlite_%' "
        "ORDER BY type, name;"
    };
    std::vector<SchemaObjectShape> objects;
    while (statement.next_row("cannot inspect state database schema objects")) {
        objects.push_back({
            .type = column_text(statement.get(), 0),
            .name = column_text(statement.get(), 1),
            .table = column_text(statement.get(), 2),
        });
    }
    return objects;
}

void verify_database_integrity(sqlite3* database) {
    try {
        Statement statement{database, "PRAGMA quick_check;"};
        if (statement.step() != SQLITE_ROW) {
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
        if (statement.step() != SQLITE_DONE) {
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
        const int result = integrity.step();
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
    if (foreign_keys.step() != SQLITE_DONE) {
        if (!detail.empty()) {
            detail += "; ";
        }
        detail += "foreign key constraint violation";
    }
    return detail.empty() ? "ok" : detail;
}

void configure_writable_database(sqlite3* database) {
    execute_sql(database, "PRAGMA journal_mode = WAL;");
    execute_sql(database, "PRAGMA synchronous = FULL;");
    execute_sql(database, "PRAGMA secure_delete = FAST;");
    execute_sql(database, "PRAGMA wal_autocheckpoint = 1000;");
    const auto maximum_pages =
        "PRAGMA max_page_count = " + std::to_string(maximum_database_pages) +
        ";";
    execute_sql(database, maximum_pages.c_str());
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
        const std::string message = reference == nullptr
                                        ? "unknown SQLite error"
                                        : sqlite3_errmsg(reference.get());
        throw std::runtime_error(
            "cannot create reference state database schema: " + message
        );
    }
    ensure_current_schema(reference.get());

    if (user_schema_objects(database) != user_schema_objects(reference.get())) {
        throw std::runtime_error(
            "state database schema contains incompatible views or triggers"
        );
    }

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
                actual_columns, expected.name, &ColumnShape::name
            );
            if (actual == actual_columns.end()) {
                throw std::runtime_error(
                    "state database table '" + table + "' is missing column '" +
                    expected.name + "'"
                );
            }
            if (!column_is_compatible(*actual, expected)) {
                throw std::runtime_error(
                    "state database table '" + table + "' column '" +
                    expected.name + "' has an incompatible definition"
                );
            }
        }
        for (const auto& actual : actual_columns) {
            if (!std::ranges::contains(
                    expected_columns, actual.name, &ColumnShape::name
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
        if (table_check_constraints(database, table) !=
            table_check_constraints(reference.get(), table)) {
            throw std::runtime_error(
                "state database table '" + table +
                "' has incompatible CHECK constraints"
            );
        }
    }
}

} // namespace onedrive::storage::item_database_detail
