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
    void operator()(sqlite3* handle) const noexcept {
        sqlite3_close(handle);
    }
};

struct SqliteStatementFinalizer {
    void operator()(sqlite3_stmt* statement) const noexcept {
        sqlite3_finalize(statement);
    }
};

using SqliteHandle = std::unique_ptr<sqlite3, SqliteCloser>;
using SqliteStatement =
    std::unique_ptr<sqlite3_stmt, SqliteStatementFinalizer>;

void execute(sqlite3* database, const char* sql);

class Statement {
public:
    Statement(gsl::not_null<sqlite3*> database, const char* sql)
        : database_{database} {
        sqlite3_stmt* statement = nullptr;
        const int result = sqlite3_prepare_v2(
            database_.get(),
            sql,
            -1,
            &statement,
            nullptr
        );
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

    [[nodiscard]] sqlite3_stmt* get() const noexcept {
        return statement_.get();
    }

private:
    gsl::not_null<sqlite3*> database_;
    SqliteStatement statement_;
};

void bind_text(
    sqlite3* database,
    sqlite3_stmt* statement,
    int index,
    std::string_view value
);
void bind_integer(
    sqlite3* database,
    sqlite3_stmt* statement,
    int index,
    std::int64_t value
);
void bind_blob(
    sqlite3* database,
    sqlite3_stmt* statement,
    int index,
    const std::vector<std::uint8_t>& value
);
[[nodiscard]] std::string column_text(
    sqlite3_stmt* statement,
    int column
);
void require_pragma_value(
    sqlite3* database,
    const char* sql,
    std::string_view expected,
    std::string_view description
);

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

}  // namespace onedrive::storage::item_database_detail
