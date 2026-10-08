#include "support.hpp"
#include "db/support.hpp"
#include "app/preflight.hpp"

namespace {

using namespace onedrive::test::app;
using namespace onedrive::test::db;

int test_migration_command() {
    CliFixture fixture;
    if (!create_current_database(fixture.state_path / "current")) {
        return fail("cannot create already-current migration fixture");
    }
    const auto legacy_directory = fixture.state_path / "legacy";
    std::filesystem::create_directory(legacy_directory);
    const auto legacy_path = legacy_directory / "items.sqlite3";
    if (!create_version_five_database(legacy_path)) {
        return fail("cannot create second outdated database");
    }
    std::filesystem::permissions(
        legacy_path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
    );
    const auto path = fixture.state_path / "items.sqlite3";
    if (!create_version_twenty_four_database(path)) {
        return fail("cannot create command migration fixture");
    }
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
    );
    const auto run = [&](std::vector<std::string> options) {
        std::vector<std::string> arguments{
            "onedrive-cpp",
            "state",
            "migrate",
            "--config",
            fixture.config_path.string(),
            "--output",
            "json"
        };
        arguments.insert(arguments.end(), options.begin(), options.end());
        return run_application(fixture.runtime_factory, std::move(arguments));
    };
    const auto preview = run({"--dry-run", "--yes"});
    if (preview.exit_code != 0 ||
        !preview.standard_output.contains(R"("schema_version":"24")") ||
        !preview.standard_output.contains(R"("latest_schema_version":"26")") ||
        !preview.standard_output.contains(R"("status":"upgrade-required")") ||
        !schema_version_is(path, 24)) {
        return fail("migration preview changed state or omitted versions");
    }
    for (const auto& entry :
         std::filesystem::directory_iterator{fixture.state_path}) {
        if (entry.path().filename().string().contains(".pre-migrate-")) {
            return fail("dry-run created a migration backup");
        }
    }
    const auto health = run_application(
        fixture.runtime_factory,
        {"onedrive-cpp",
         "inspect",
         "health",
         "--config",
         fixture.config_path.string(),
         "--output",
         "json"}
    );
    if (health.exit_code != 1 ||
        !health.standard_output.contains(R"("status":"upgrade-required")") ||
        !health.standard_output.contains(R"("schema_version":"24")") ||
        !health.standard_output.contains(R"("latest_schema_version":"26")") ||
        !health.standard_output.contains("database_upgrade_required")) {
        return fail(
            "health did not distinguish an old database from corruption"
        );
    }
    const auto original_config = onedrive::test::read_file(fixture.config_path);
    auto dry_config = original_config;
    dry_config.insert(dry_config.find("[sync]\n") + 7, "dry_run = true\n");
    onedrive::test::write_file(fixture.config_path, dry_config);
    const auto configured_preview = run({"--yes"});
    onedrive::test::write_file(fixture.config_path, original_config);
    if (configured_preview.exit_code != 0 || !schema_version_is(path, 24)) {
        return fail("configured dry-run did not prevent migration");
    }
    const auto declined = run({});
    if (declined.exit_code != 1 ||
        !declined.standard_error.contains("confirmation_required") ||
        !schema_version_is(path, 24)) {
        return fail("JSON migration did not require explicit confirmation");
    }
    {
        const auto cancelled = run_application(
            fixture.runtime_factory,
            {"onedrive-cpp",
             "state",
             "migrate",
             "--config",
             fixture.config_path.string()},
            "no\n"
        );
        if (cancelled.exit_code != 1 || !schema_version_is(path, 24) ||
            !cancelled.standard_output.contains("cancelled")) {
            return fail("text migration did not respect declined confirmation");
        }
    }
    const auto invalid_directory = fixture.state_path / "invalid";
    std::filesystem::create_directory(invalid_directory);
    const auto invalid_path = invalid_directory / "items.sqlite3";
    if (!create_version_twenty_four_database(invalid_path) ||
        !execute_schema(invalid_path, "PRAGMA user_version = 27;")) {
        return fail("cannot create unsupported command fixture");
    }
    std::filesystem::permissions(
        invalid_path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
    );
    const auto unsupported = run({"--yes"});
    if (unsupported.exit_code != 1 || !schema_version_is(path, 24) ||
        !unsupported.standard_output.contains(R"("schema_version":"27")")) {
        return fail(
            "migration changed an earlier database despite failed prechecks"
        );
    }
    std::filesystem::remove(invalid_path);
    std::filesystem::remove(invalid_directory);
    {
        const auto config = onedrive::config::Config::load(fixture.config_path);
        const onedrive::app::detail::RuntimePreflight lock{
            config, onedrive::app::detail::Operation::migrate_state
        };
        const auto locked = run({"--yes"});
        if (locked.exit_code != 1 ||
            !locked.standard_error.contains(
                "already using this state directory"
            ) ||
            !schema_version_is(path, 24)) {
            return fail("migration did not honor the shared runtime lock");
        }
    }
    const auto migrated = run({"--yes"});
    if (migrated.exit_code != 0 || !schema_version_is(path, 26) ||
        !schema_version_is(legacy_path, 26) ||
        !migrated.standard_output.contains("database_migration_completed") ||
        !migrated.standard_output.contains("backup_path") ||
        std::filesystem::exists(fixture.sync_path)) {
        return fail("offline migration failed or touched the sync directory");
    }
    const auto repeated = run({"--yes"});
    if (repeated.exit_code != 0 ||
        !repeated.standard_output.contains("No databases require an upgrade")) {
        return fail("repeated migration did not skip current databases");
    }
    if (!execute_schema(
            path,
            "ALTER TABLE item DROP COLUMN content_hash_algorithm;"
            "ALTER TABLE item DROP COLUMN content_hash_value;"
            "PRAGMA user_version = 24;"
            "CREATE TABLE unexpected(value TEXT);"
        )) {
        return fail("cannot create structurally invalid command fixture");
    }
    const auto failed = run({"--yes"});
    if (failed.exit_code != 1 || !schema_version_is(path, 24) ||
        !failed.standard_error.contains("backup retained")) {
        return fail("migration failure did not report the retained backup");
    }
    onedrive::test::write_file(path, "corrupt");
    const auto invalid = run({"--yes"});
    if (invalid.exit_code != 1 ||
        !invalid.standard_error.contains("database_migration_failed") ||
        onedrive::test::read_file(path) != "corrupt") {
        return fail("migration command did not reject corruption safely");
    }
    return EXIT_SUCCESS;
}

int test_empty_and_confirmed_migration() {
    CliFixture fixture;
    const auto run = [&](std::string input = {}) {
        return run_application(
            fixture.runtime_factory,
            {"onedrive-cpp",
             "state",
             "migrate",
             "--config",
             fixture.config_path.string(),
             "--color",
             "never"},
            std::move(input)
        );
    };
    const auto empty = run();
    if (empty.exit_code != 0 ||
        !empty.standard_output.contains("No databases require an upgrade")) {
        return fail("empty state directory required migration or confirmation");
    }
    const auto path = fixture.state_path / "items.sqlite3";
    if (!create_version_five_database(path)) {
        return fail("cannot create confirmed migration fixture");
    }
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
    );
    const auto confirmed = run("migrate\n");
    if (confirmed.exit_code != 0 || !schema_version_is(path, 26) ||
        !confirmed.standard_output.contains("Database upgrades completed")) {
        return fail("explicit text confirmation did not upgrade the database");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (test_migration_command() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return test_empty_and_confirmed_migration();
}
