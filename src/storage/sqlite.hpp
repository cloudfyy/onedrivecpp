#pragma once

#include <gsl/pointers>
#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace onedrive::storage::item_database_detail {

struct SqliteCloser {
    void operator()(sqlite3* handle) const noexcept;
};

struct SqliteStatementFinalizer {
    void operator()(sqlite3_stmt* statement) const noexcept;
};

using SqliteHandle = std::unique_ptr<sqlite3, SqliteCloser>;
using SqliteStatement = std::unique_ptr<sqlite3_stmt, SqliteStatementFinalizer>;
using Database = gsl::not_null<sqlite3*>;
using PreparedStatement = gsl::not_null<sqlite3_stmt*>;
using Sql = gsl::not_null<const char*>;

void execute(Database database, Sql sql);

class Statement {
public:
    Statement(Database database, Sql sql)
        : database_{database} {
        sqlite3_stmt* statement = nullptr;
        const int result =
            sqlite3_prepare_v2(database_.get(), sql, -1, &statement, nullptr);
        statement_.reset(statement);
        if (result != SQLITE_OK) {
            throw std::runtime_error(
                "cannot prepare SQLite statement: " +
                std::string{sqlite3_errmsg(database_.get())}
            );
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&&) = delete;
    Statement& operator=(Statement&&) = delete;

    [[nodiscard]] PreparedStatement get() const noexcept {
        return PreparedStatement{statement_.get()};
    }

    [[nodiscard]] int step() noexcept {
        return sqlite3_step(statement_.get());
    }

    [[nodiscard]] bool next(std::string_view operation) {
        const int result = step();
        if (result == SQLITE_ROW) {
            return true;
        }
        if (result == SQLITE_DONE) {
            return false;
        }
        throw_error(operation);
    }

    void step_done(std::string_view operation) {
        if (step() != SQLITE_DONE) {
            throw_error(operation);
        }
    }

    void step_row(std::string_view operation) {
        if (step() != SQLITE_ROW) {
            throw_error(operation);
        }
    }

    void reset_for_reuse() {
        const int reset_result = sqlite3_reset(statement_.get());
        const int clear_result = sqlite3_clear_bindings(statement_.get());
        if (reset_result != SQLITE_OK || clear_result != SQLITE_OK) {
            throw_error("cannot reset SQLite statement for reuse");
        }
    }

private:
    [[noreturn]] void throw_error(std::string_view operation) const {
        throw std::runtime_error(
            std::string{operation} + ": " + sqlite3_errmsg(database_.get())
        );
    }

    Database database_;
    SqliteStatement statement_;
};

void bind_text(
    Database database,
    PreparedStatement statement,
    int index,
    std::string_view value
);
void bind_integer(
    Database database,
    PreparedStatement statement,
    int index,
    std::int64_t value
);
void bind_blob(
    Database database,
    PreparedStatement statement,
    int index,
    const std::vector<std::uint8_t>& value
);
[[nodiscard]] std::string column_text(
    PreparedStatement statement,
    int column
);
void require_pragma_value(
    Database database,
    Sql sql,
    std::string_view expected,
    std::string_view description
);

class Transaction {
public:
    explicit Transaction(Database database)
        : database_{database} {
        execute(database_.get(), "BEGIN IMMEDIATE;");
    }

    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    void commit() {
        execute(database_.get(), "COMMIT;");
        committed_ = true;
    }

private:
    Database database_;
    bool committed_{false};
};

} // namespace onedrive::storage::item_database_detail
