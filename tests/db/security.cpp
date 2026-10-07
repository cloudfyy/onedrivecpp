#include "support.hpp"
#include "onedrive/storage/item_database.hpp"
#include "support/common.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace onedrive::test::db;
using onedrive::test::fail;
using onedrive::test::TemporaryDirectory;

} // namespace

int main() {
    TemporaryDirectory temporary_directory;
    const auto assert_rejected = [](const std::filesystem::path& directory,
                                    std::string_view expected,
                                    std::string_view description) {
        const auto error = database_open_error(directory);
        if (!error || !error->contains(expected) ||
            !error->contains("Move or remove this database file")) {
            return fail(
                std::string{description} +
                " was not rejected with recovery guidance"
            );
        }
        return EXIT_SUCCESS;
    };

    const auto missing_table_directory =
        temporary_directory.path() / "schema-missing-table";
    if (!create_current_database(missing_table_directory) ||
        !execute_schema(
            missing_table_directory / "items.sqlite3",
            "DROP TABLE pending_delete;"
        )) {
        return fail("could not create missing-table database fixture");
    }
    const auto missing_table_diagnostics =
        onedrive::storage::diagnose_state_databases(missing_table_directory);
    if (missing_table_diagnostics.size() != 1 ||
        missing_table_diagnostics.front().healthy ||
        !missing_table_diagnostics.front().detail.contains(
            "missing table 'pending_delete'"
        ) ||
        assert_rejected(
            missing_table_directory,
            "missing table 'pending_delete'",
            "database with a missing table"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto missing_column_directory =
        temporary_directory.path() / "schema-missing-column";
    if (!create_current_database(missing_column_directory) ||
        !execute_schema(
            missing_column_directory / "items.sqlite3",
            "ALTER TABLE item DROP COLUMN ctag;"
        ) ||
        assert_rejected(
            missing_column_directory,
            "missing column 'ctag'",
            "database with a missing column"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto extra_column_directory =
        temporary_directory.path() / "schema-extra-column";
    if (!create_current_database(extra_column_directory) ||
        !execute_schema(
            extra_column_directory / "items.sqlite3",
            "ALTER TABLE drive_state ADD COLUMN unexpected TEXT;"
        ) ||
        assert_rejected(
            extra_column_directory,
            "unexpected column 'unexpected'",
            "database with an extra column"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto extra_table_directory =
        temporary_directory.path() / "schema-extra-table";
    if (!create_current_database(extra_table_directory) ||
        !execute_schema(
            extra_table_directory / "items.sqlite3",
            "CREATE TABLE unexpected (value TEXT);"
        ) ||
        assert_rejected(
            extra_table_directory,
            "unexpected table 'unexpected'",
            "database with an extra table"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto extra_index_directory =
        temporary_directory.path() / "schema-extra-index";
    if (!create_current_database(extra_index_directory) ||
        !execute_schema(
            extra_index_directory / "items.sqlite3",
            "CREATE INDEX unexpected_item_name ON item(name);"
        ) ||
        assert_rejected(
            extra_index_directory,
            "incompatible primary key or index definitions",
            "database with an extra index"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto wrong_default_directory =
        temporary_directory.path() / "schema-wrong-default";
    if (!create_current_database(wrong_default_directory) ||
        !execute_schema(
            wrong_default_directory / "items.sqlite3",
            "DROP TABLE drive_state;"
            "CREATE TABLE drive_state ("
            "drive_id TEXT PRIMARY KEY NOT NULL, delta_link TEXT NOT NULL, "
            "sync_filter_fingerprint TEXT NOT NULL DEFAULT 'wrong');"
        ) ||
        assert_rejected(
            wrong_default_directory,
            "column 'sync_filter_fingerprint' has an incompatible definition",
            "database with an incorrect column default"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto missing_check_directory =
        temporary_directory.path() / "schema-missing-check";
    if (!create_current_database(missing_check_directory) ||
        !execute_schema(
            missing_check_directory / "items.sqlite3",
            "DROP TABLE identity;"
            "CREATE TABLE identity ("
            "singleton INTEGER PRIMARY KEY NOT NULL, user_id TEXT NOT NULL, "
            "user_display_name TEXT NOT NULL, drive_id TEXT NOT NULL, "
            "drive_name TEXT NOT NULL, avatar_content_type TEXT NOT NULL, "
            "avatar_bytes BLOB NOT NULL);"
        ) ||
        assert_rejected(
            missing_check_directory,
            "incompatible CHECK constraints",
            "database with a missing CHECK constraint"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto unexpected_trigger_directory =
        temporary_directory.path() / "schema-unexpected-trigger";
    if (!create_current_database(unexpected_trigger_directory) ||
        !execute_schema(
            unexpected_trigger_directory / "items.sqlite3",
            "CREATE TRIGGER unexpected_item_insert AFTER INSERT ON item "
            "BEGIN DELETE FROM item WHERE drive_id = NEW.drive_id; END;"
        ) ||
        assert_rejected(
            unexpected_trigger_directory,
            "incompatible views or triggers",
            "database with an unexpected trigger"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto wrong_index_directory =
        temporary_directory.path() / "schema-wrong-index";
    if (!create_current_database(wrong_index_directory) ||
        !execute_schema(
            wrong_index_directory / "items.sqlite3",
            "DROP INDEX item_drive_remote_path;"
            "CREATE INDEX item_drive_remote_path "
            "ON item(drive_id, remote_path COLLATE NOCASE DESC);"
        ) ||
        assert_rejected(
            wrong_index_directory,
            "incompatible primary key or index definitions",
            "database with an incompatible index definition"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto wrong_primary_key_directory =
        temporary_directory.path() / "schema-wrong-primary-key";
    if (!create_current_database(wrong_primary_key_directory) ||
        !execute_schema(
            wrong_primary_key_directory / "items.sqlite3",
            "DROP TABLE upload_suppression;"
            "CREATE TABLE upload_suppression ("
            "drive_id TEXT NOT NULL, remote_id TEXT NOT NULL, "
            "local_path TEXT NOT NULL, source_device INTEGER NOT NULL, "
            "source_inode INTEGER NOT NULL, "
            "PRIMARY KEY (drive_id, remote_id));"
        ) ||
        assert_rejected(
            wrong_primary_key_directory,
            "incompatible definition",
            "database with an incorrect primary key"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto future_version_directory =
        temporary_directory.path() / "schema-future-version";
    if (!create_current_database(future_version_directory) ||
        !execute_schema(
            future_version_directory / "items.sqlite3",
            "PRAGMA user_version = 27;"
        ) ||
        assert_rejected(
            future_version_directory,
            "unsupported state database schema version 27",
            "database with a future schema version"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto hard_link_directory =
        temporary_directory.path() / "schema-hard-link";
    if (!create_current_database(hard_link_directory)) {
        return fail("could not create hard-link database fixture");
    }
    std::filesystem::create_hard_link(
        hard_link_directory / "items.sqlite3",
        hard_link_directory / "items.sqlite3.alias"
    );
    const auto hard_link_error = database_open_error(hard_link_directory);
    if (!hard_link_error || !hard_link_error->contains(
                                "single-link file owned by the current user"
                            )) {
        return fail("hard-linked database was not rejected");
    }

    const auto insecure_mode_directory =
        temporary_directory.path() / "schema-insecure-mode";
    if (!create_current_database(insecure_mode_directory)) {
        return fail("could not create insecure-mode database fixture");
    }
    std::filesystem::permissions(
        insecure_mode_directory, std::filesystem::perms::all
    );
    std::filesystem::permissions(
        insecure_mode_directory / "items.sqlite3", std::filesystem::perms::all
    );
    const auto insecure_mode_diagnostics =
        onedrive::storage::diagnose_state_databases(insecure_mode_directory);
    if (insecure_mode_diagnostics.size() != 1 ||
        insecure_mode_diagnostics.front().healthy ||
        !insecure_mode_diagnostics.front().detail.contains("mode 0600")) {
        return fail("database diagnostics accepted insecure permissions");
    }
    {
        onedrive::storage::ItemDatabase secured{
            insecure_mode_directory, identity()
        };
        secured.open();
    }
    if ((std::filesystem::status(insecure_mode_directory).permissions() &
         std::filesystem::perms::all) != std::filesystem::perms::owner_all ||
        (std::filesystem::status(insecure_mode_directory / "items.sqlite3")
             .permissions() &
         std::filesystem::perms::all) !=
            (std::filesystem::perms::owner_read |
             std::filesystem::perms::owner_write)) {
        return fail("insecure database permissions were not repaired");
    }

    const auto symlink_directory =
        temporary_directory.path() / "schema-symlink";
    std::filesystem::create_directories(symlink_directory);
    std::filesystem::create_symlink(
        insecure_mode_directory / "items.sqlite3",
        symlink_directory / "items.sqlite3"
    );
    const auto symlink_error = database_open_error(symlink_directory);
    const auto symlink_diagnostics =
        onedrive::storage::diagnose_state_databases(symlink_directory);
    if (!symlink_error || symlink_diagnostics.size() != 1 ||
        symlink_diagnostics.front().healthy ||
        !symlink_diagnostics.front().detail.contains(
            "not a regular non-symbolic-link file"
        ) ||
        !symlink_error->contains("cannot safely open path")) {
        return fail("symbolic-link database was not rejected");
    }

    const auto unversioned_directory =
        temporary_directory.path() / "schema-unversioned";
    std::filesystem::create_directories(unversioned_directory);
    if (!execute_schema(
            unversioned_directory / "items.sqlite3",
            "CREATE TABLE unexpected (value TEXT);"
        ) ||
        assert_rejected(
            unversioned_directory,
            "unversioned state database contains existing tables",
            "unversioned non-empty database"
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    const auto interrupted_directory =
        temporary_directory.path() / "schema-interrupted-migration";
    const auto interrupted_path = interrupted_directory / "items.sqlite3";
    std::filesystem::create_directories(interrupted_directory);
    if (!create_version_twenty_two_database(interrupted_path) ||
        !execute_schema(
            interrupted_path,
            "ALTER TABLE pending_upload ADD COLUMN failure_code "
            "TEXT NOT NULL DEFAULT '';"
        ) ||
        assert_rejected(
            interrupted_directory,
            "duplicate column name: failure_code",
            "partially applied migration"
        ) != EXIT_SUCCESS ||
        !schema_version_is(interrupted_path, 22)) {
        return fail(
            "partially applied migration changed the saved schema version"
        );
    }

    const auto corrupt_directory =
        temporary_directory.path() / "schema-corrupt";
    std::filesystem::create_directories(corrupt_directory);
    {
        std::ofstream corrupt{
            corrupt_directory / "items.sqlite3", std::ios::binary
        };
        corrupt << "not a SQLite database";
    }
    const auto corrupt_diagnostics =
        onedrive::storage::diagnose_state_databases(corrupt_directory);
    if (corrupt_diagnostics.size() != 1 ||
        corrupt_diagnostics.front().healthy ||
        corrupt_diagnostics.front().detail.empty()) {
        return fail("full integrity diagnostics accepted a corrupt database");
    }
    {
        onedrive::storage::ItemDatabase rebuilt{corrupt_directory, identity()};
        rebuilt.open();
        if (rebuilt.size() != 0) {
            return fail("rebuilt database was not empty");
        }
    }
    std::filesystem::path quarantined;
    for (const auto& entry :
         std::filesystem::directory_iterator{corrupt_directory}) {
        if (entry.path().filename().string().starts_with(
                "items.sqlite3.corrupt-"
            ) &&
            !entry.path().filename().string().ends_with("-wal") &&
            !entry.path().filename().string().ends_with("-shm")) {
            quarantined = entry.path();
            break;
        }
    }
    if (quarantined.empty()) {
        return fail("corrupt database evidence was not quarantined");
    }
    std::ifstream evidence{quarantined, std::ios::binary};
    const std::string evidence_contents{
        std::istreambuf_iterator<char>{evidence},
        std::istreambuf_iterator<char>{}
    };
    const auto rebuilt_diagnostics =
        onedrive::storage::diagnose_state_databases(corrupt_directory);
    if (evidence_contents != "not a SQLite database" ||
        rebuilt_diagnostics.size() != 1 ||
        !rebuilt_diagnostics.front().healthy ||
        rebuilt_diagnostics.front().detail != "ok") {
        return fail("corrupt database quarantine or rebuild was incomplete");
    }

    return EXIT_SUCCESS;
}
