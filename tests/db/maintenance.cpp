#include "support.hpp"
#include "support/common.hpp"
#include "onedrive/storage/item_database.hpp"
#include "storage/sqlite.hpp"

#include <filesystem>
#include <string>

namespace {

using namespace onedrive::test;
using namespace onedrive::test::db;
using namespace onedrive::storage;
using namespace onedrive::storage::item_database_detail;

SqliteHandle open_fixture(const std::filesystem::path& path) {
    sqlite3* handle = nullptr;
    const auto result = sqlite3_open(path.c_str(), &handle);
    SqliteHandle database{handle};
    if (result != SQLITE_OK) {
        throw std::runtime_error("cannot open maintenance fixture");
    }
    return database;
}

std::string cursor(const std::filesystem::path& path) {
    auto database = open_fixture(path);
    Statement query{
        database.get(),
        "SELECT delta_link FROM drive_state WHERE drive_id = 'me';"
    };
    query.require_row("cannot read fixture cursor");
    return column_text(query.get(), 0);
}

int test_migration() {
    TemporaryDirectory temporary;
    for (const int version : {5, 24}) {
        const auto directory = temporary.path() / std::to_string(version);
        std::filesystem::create_directory(directory);
        const auto path = directory / "items.sqlite3";
        if (!(version == 5 ? create_version_five_database(path)
                           : create_version_twenty_four_database(path))) {
            return fail("cannot create maintenance fixture");
        }
        std::filesystem::permissions(
            path,
            std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write
        );
        auto writer = open_fixture(path);
        execute_sql(writer.get(), "PRAGMA journal_mode = WAL;");
        execute_sql(writer.get(), "PRAGMA wal_autocheckpoint = 0;");
        execute_sql(
            writer.get(),
            "INSERT INTO drive_state(drive_id, delta_link) VALUES ('me', "
            "'saved-cursor');"
        );
        const auto before = read_file(path);
        const auto diagnosed = diagnose_state_databases(directory);
        if (diagnosed.size() != 1 || diagnosed[0].healthy ||
            !diagnosed[0].migration_required ||
            diagnosed[0].schema_version != version ||
            diagnosed[0].latest_schema_version != 26 ||
            read_file(path) != before || !schema_version_is(path, version)) {
            return fail(
                "read-only diagnostics did not identify old schema versions"
            );
        }
        const auto backup = migrate_state_database(path);
        if (backup.empty() || !schema_version_is(backup, version) ||
            !schema_version_is(path, 26) || cursor(backup) != "saved-cursor" ||
            cursor(path) != "saved-cursor" ||
            (std::filesystem::status(backup).permissions() &
             std::filesystem::perms::all) !=
                (std::filesystem::perms::owner_read |
                 std::filesystem::perms::owner_write)) {
            return fail(
                "migration did not preserve data or securely back up WAL "
                "contents"
            );
        }
        const auto after = diagnose_state_databases(directory);
        if (after.size() != 1 || !after[0].healthy ||
            after[0].schema_version != 26 || after[0].migration_required ||
            !migrate_state_database(path).empty()) {
            return fail("migration was not healthy and idempotent");
        }
    }
    return EXIT_SUCCESS;
}

int test_failures() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "items.sqlite3";
    if (!create_version_twenty_four_database(path)) {
        return fail("cannot create failed migration fixture");
    }
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
    );
    if (!execute_schema(
            path,
            "CREATE TABLE parent(id INTEGER PRIMARY KEY);"
            "CREATE TABLE child(parent_id INTEGER REFERENCES parent(id));"
            "INSERT INTO child VALUES (42);"
        )) {
        return fail("cannot create foreign-key integrity fixture");
    }
    const auto invalid = diagnose_state_databases(temporary.path());
    if (invalid.size() != 1 || invalid[0].healthy ||
        invalid[0].migration_required || invalid[0].schema_version != 24 ||
        !throws_with(
            [&] { static_cast<void>(migrate_state_database(path)); },
            "integrity check failed"
        ) ||
        !schema_version_is(path, 24)) {
        return fail(
            "migration accepted an older database with integrity failures"
        );
    }
    if (!execute_schema(path, "DROP TABLE child; DROP TABLE parent;")) {
        return fail("cannot remove foreign-key fixture tables");
    }
    // A late schema-validation failure must roll back earlier migration steps.
    if (!execute_schema(path, "CREATE TABLE unexpected(value TEXT);")) {
        return fail("cannot create incompatible migration fixture");
    }
    if (!throws_with(
            [&] { static_cast<void>(migrate_state_database(path)); },
            "backup retained"
        ) ||
        !schema_version_is(path, 24)) {
        return fail(
            "failed migration did not retain its backup and original version"
        );
    }
    {
        auto database = open_fixture(path);
        Statement columns{
            database.get(),
            "SELECT count(*) FROM pragma_table_info('item') WHERE name = "
            "'content_hash_algorithm';"
        };
        columns.require_row("cannot check rolled-back columns");
        if (sqlite3_column_int(columns.get(), 0) != 0) {
            return fail("failed migration left partial schema changes");
        }
    }
    if (!execute_schema(
            path, "DROP TABLE unexpected; PRAGMA user_version = 27;"
        )) {
        return fail("cannot create future schema fixture");
    }
    for (const int version : {27, 0}) {
        if (version == 0 && !execute_schema(path, "PRAGMA user_version = 0;")) {
            return fail("cannot create unversioned fixture");
        }
        const auto result = diagnose_state_databases(temporary.path());
        if (result.size() != 1 || result[0].healthy ||
            result[0].migration_required ||
            result[0].schema_version != version ||
            !throws_with(
                [&] { static_cast<void>(migrate_state_database(path)); },
                "unsupported state database schema version"
            )) {
            return fail("unsupported database version was accepted");
        }
    }
    write_file(path, "not a SQLite database");
    if (!throws_with([&] { static_cast<void>(migrate_state_database(path)); }
        ) ||
        read_file(path) != "not a SQLite database") {
        return fail("corrupt database was rebuilt or changed");
    }
    const auto outside = temporary.path() / "outside";
    std::filesystem::rename(path, outside);
    std::filesystem::create_symlink(outside, path);
    if (!throws_with(
            [&] { static_cast<void>(migrate_state_database(path)); },
            "not a regular"
        )) {
        return fail("migration followed a database symlink");
    }
    std::filesystem::remove(path);
    std::filesystem::create_hard_link(outside, path);
    if (!throws_with(
            [&] { static_cast<void>(migrate_state_database(path)); },
            "single-link"
        )) {
        return fail("migration accepted a hard-linked database");
    }
    std::filesystem::remove(path);
    std::filesystem::rename(outside, path);
    std::filesystem::permissions(path, std::filesystem::perms::all);
    if (!throws_with(
            [&] { static_cast<void>(migrate_state_database(path)); }, "0600"
        )) {
        return fail("migration accepted insecure database permissions");
    }
    return EXIT_SUCCESS;
}

int test_unsafe_sidecars() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "items.sqlite3";
    if (!throws_with(
            [&] { static_cast<void>(migrate_state_database(path)); },
            "cannot inspect database security"
        ) ||
        std::filesystem::exists(path)) {
        return fail("migration created a missing database");
    }
    if (!create_version_twenty_four_database(path)) {
        return fail("cannot create sidecar migration fixture");
    }
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
    );
    const auto outside = temporary.path() / "outside";
    write_file(outside, "must not change");
    for (const auto* suffix : {"-wal", "-shm"}) {
        const std::filesystem::path sidecar{path.string() + suffix};
        std::filesystem::create_symlink(outside, sidecar);
        if (!throws_with(
                [&] { static_cast<void>(migrate_state_database(path)); },
                "not a regular"
            ) ||
            read_file(outside) != "must not change") {
            return fail("migration followed an unsafe SQLite sidecar");
        }
        std::filesystem::remove(sidecar);
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (test_migration() != EXIT_SUCCESS || test_failures() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return test_unsafe_sidecars();
}
