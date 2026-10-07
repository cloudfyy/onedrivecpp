#include "storage/sqlite.hpp"
#include "support/common.hpp"

#include <sqlite3.h>

#include <cstdlib>
#include <memory>

namespace {

using onedrive::storage::item_database_detail::SqliteHandle;
using onedrive::storage::item_database_detail::SqliteStatement;
using onedrive::storage::item_database_detail::Transaction;
using onedrive::storage::item_database_detail::execute;
using onedrive::test::fail;

int test_transaction_rolls_back() {
    sqlite3* raw_database = nullptr;
    if (sqlite3_open(":memory:", &raw_database) != SQLITE_OK) {
        return fail("could not open in-memory SQLite database");
    }
    SqliteHandle database{raw_database};
    execute(database.get(), "CREATE TABLE value (number INTEGER);");
    {
        Transaction transaction{database.get()};
        execute(database.get(), "INSERT INTO value VALUES (1);");
    }

    sqlite3_stmt* raw_statement = nullptr;
    if (sqlite3_prepare_v2(
            database.get(),
            "SELECT COUNT(*) FROM value;",
            -1,
            &raw_statement,
            nullptr
        ) != SQLITE_OK) {
        return fail("could not inspect rolled-back SQLite transaction");
    }
    SqliteStatement statement{raw_statement};
    if (sqlite3_step(statement.get()) != SQLITE_ROW ||
        sqlite3_column_int(statement.get(), 0) != 0) {
        return fail("SQLite transaction destructor did not roll back");
    }
    return EXIT_SUCCESS;
}

int test_close_defers_until_statements_finalize() {
    sqlite3* raw_database = nullptr;
    if (sqlite3_open(":memory:", &raw_database) != SQLITE_OK) {
        return fail("could not open deferred-close SQLite database");
    }
    SqliteHandle database{raw_database};
    sqlite3_stmt* raw_statement = nullptr;
    if (sqlite3_prepare_v2(
            database.get(), "SELECT 1;", -1, &raw_statement, nullptr
        ) != SQLITE_OK) {
        return fail("could not prepare deferred-close SQLite statement");
    }
    SqliteStatement statement{raw_statement};
    database.reset();
    if (sqlite3_step(statement.get()) != SQLITE_ROW ||
        sqlite3_column_int(statement.get(), 0) != 1) {
        return fail("SQLite close did not defer for an active statement");
    }
    statement.reset();
    return EXIT_SUCCESS;
}

int test_cleanup_failures_do_not_escape() {
    sqlite3* raw_database = nullptr;
    if (sqlite3_open(":memory:", &raw_database) != SQLITE_OK) {
        return fail("could not open cleanup-failure SQLite database");
    }
    SqliteHandle database{raw_database};
    {
        Transaction transaction{database.get()};
        execute(database.get(), "ROLLBACK;");
    }

    sqlite3_stmt* raw_statement = nullptr;
    if (sqlite3_prepare_v2(
            database.get(),
            "SELECT abs(-9223372036854775808);",
            -1,
            &raw_statement,
            nullptr
        ) != SQLITE_OK) {
        return fail("could not prepare failing SQLite statement");
    }
    SqliteStatement statement{raw_statement};
    if (sqlite3_step(statement.get()) != SQLITE_ERROR) {
        return fail("SQLite fixture did not produce a finalize error");
    }
    statement.reset();
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (const int result = test_transaction_rolls_back();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_close_defers_until_statements_finalize();
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_cleanup_failures_do_not_escape();
}
