#include "support.hpp"
#include "app/info.hpp"
#include "db/support.hpp"
#include "onedrive/logging/logging.hpp"
#include "sync/filesystem/operations.hpp"

#include <functional>
#include <unistd.h>

namespace {

using namespace onedrive::test::app;
using onedrive::test::read_file;
using onedrive::test::write_file;
namespace storage = onedrive::storage;

struct CleanupFixture {
    CliFixture cli;
    onedrive::account::DriveIdentity identity{
        onedrive::test::test_drive_identity("me")
    };
    std::filesystem::path root;
    std::filesystem::path database_directory;

    CleanupFixture() {
        if (cli.authenticate().exit_code != 0) {
            throw std::runtime_error("cleanup fixture authentication failed");
        }
        cli.runtime_factory.use_real_item_store = true;
        root = onedrive::account::AccountState::drive_data_directory(
            cli.sync_path, identity
        );
        database_directory =
            onedrive::account::AccountState::locate(cli.state_path, identity)
                .drive_directory;
        std::filesystem::create_directories(root);
    }

    void database(const std::function<void(storage::ItemDatabase&)>& action) {
        const onedrive::logging::Session logging{{.level = "off"}};
        storage::ItemDatabase db{database_directory, identity};
        db.open();
        action(db);
    }

    storage::PartialDownload partial(
        std::string name,
        std::uint64_t completed,
        std::int64_t total,
        std::optional<std::string> contents
    ) {
        storage::PartialDownload result{
            .item =
                {
                    .drive_id = identity.drive_id,
                    .remote_id = name,
                    .name = name,
                    .remote_path = name,
                    .local_path = root / name,
                    .size = total,
                },
            .temporary_path =
                onedrive::sync::detail::temporary_path_for(root / name),
            .completed_bytes = completed,
        };
        if (contents) {
            write_file(result.temporary_path, *contents);
        }
        database([&](auto& db) { db.save_partial_download(result); });
        return result;
    }

    RunResult run(std::vector<std::string> options, std::string input = {}) {
        std::vector<std::string> arguments{
            "onedrive-cpp",
            "state",
            "cleanup",
            "--config",
            cli.config_path.string(),
        };
        arguments.insert(arguments.end(), options.begin(), options.end());
        return run_application(
            cli.runtime_factory, std::move(arguments), std::move(input)
        );
    }

    bool has_record(const storage::PartialDownload& partial) {
        bool present = false;
        database([&](auto& db) {
            present =
                db.partial_download(identity.drive_id, partial.item.remote_id)
                    .has_value();
        });
        return present;
    }
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

void test_cleanup() {
    CleanupFixture fixture;
    const auto good = fixture.partial("good.bin", 2, 8, "more");
    const auto missing = fixture.partial("missing.bin", 2, 8, std::nullopt);
    const auto short_file = fixture.partial("short.bin", 4, 8, "xx");
    const auto invalid = fixture.partial("invalid.bin", 9, 9, "xxx");
    const auto negative = fixture.partial("negative.bin", 0, 1, "x");
    const auto orphan = fixture.root / ".orphan.bin.onedrive-partial-9-9";
    write_file(orphan, "orphan");

    const std::vector<std::string> ordinary{
        "file.txt",
        "file.onedrive-partial-1-2",
        ".file.onedrive-partial-",
        ".file.onedrive-partial-1",
        ".file.onedrive-partial-1-2.backup",
        ".file.onedrive-partial-x-2",
        ".file.onedrive-partial-1-x",
        ".file.onedrive-partial-0-1",
        ".file.onedrive-partial-1-0",
        ".file.onedrive-partial--1-2",
        ".file.onedrive-partial-1-2-3",
        ".file.onedrive-partial-01-2",
        ".onedrive-partial-1-2",
        ".file.onedrive-partial-1-",
        ".file.onedrive-partial--2",
        ".file.onedrive-partial-1.-2",
        ".ordinary-hidden-file",
        ".file.onedrive-partial-1x-2",
    };
    for (const auto& name : ordinary) {
        write_file(fixture.root / name, "keep");
    }
    const auto protected_path = fixture.root / ".tracked.onedrive-partial-1-2";
    const auto pending_path = fixture.root / ".pending.onedrive-partial-1-2";
    const auto other_drive = fixture.root.parent_path() / "other-drive";
    std::filesystem::create_directory(other_drive);
    write_file(other_drive / orphan.filename(), "other");
    write_file(protected_path, "tracked");
    write_file(pending_path, "pending");
    fixture.database([&](auto& db) {
        auto item = good.item;
        item.remote_id = "tracked";
        item.local_path = protected_path;
        db.upsert(item);
        item.remote_id = "pending";
        item.local_path = fixture.root / "pending";
        db.save_pending_download({
            .item = item,
            .temporary_path = pending_path,
            .content_fingerprint = "pending-content",
        });
    });
    require(
        onedrive::test::db::execute_schema(
            fixture.database_directory / "items.sqlite3",
            "UPDATE partial_download SET size = 8 WHERE remote_id = "
            "'invalid.bin';"
            "UPDATE partial_download SET size = -1 WHERE remote_id = "
            "'negative.bin';"
        ),
        "cannot prepare invalid recorded size fixtures"
    );

    const auto original = read_file(fixture.cli.config_path);
    auto config = original;
    config.insert(config.find("[sync]\n") + 7, "dry_run = true\n");
    write_file(fixture.cli.config_path, config);
    const auto configured = fixture.run({"--yes", "--output", "json"});
    require(
        configured.exit_code == 0 &&
            configured.standard_output.contains(R"("mode":"dry-run")") &&
            configured.standard_output.contains(R"("planned_files":"4")") &&
            configured.standard_output.contains(R"("record_only":"1")") &&
            configured.standard_output.contains(R"("planned_bytes":"12")") &&
            configured.standard_output.contains(
                R"("remote_path":"missing.bin")"
            ) &&
            configured.standard_output.contains(R"("status":"missing")") &&
            configured.standard_output.contains(R"("temporary_path":)"),
        "configured preview did not report the complete cleanup plan"
    );
    require(
        std::filesystem::exists(orphan) && fixture.has_record(missing),
        "configured dry-run removed files or database records"
    );
    write_file(fixture.cli.config_path, original);
    require(
        fixture.run({"--dry-run", "--yes", "--output", "json"}).exit_code ==
                0 &&
            fixture.has_record(short_file),
        "explicit dry-run changed records"
    );
    require(
        fixture.run({"--output", "json"}).exit_code == 1 &&
            fixture.has_record(invalid),
        "JSON cleanup bypassed confirmation"
    );
    require(
        fixture.run({}, "no\n").exit_code == 1 &&
            std::filesystem::exists(orphan),
        "cancelled cleanup removed files"
    );

    const auto result = fixture.run({}, "me\n");
    require(
        result.exit_code == 0 &&
            result.standard_output.contains(
                "Removed 4 invalid partial records and 4 temporary files"
            ),
        "confirmed cleanup did not remove all safe candidates"
    );
    require(
        fixture.has_record(good) &&
            std::filesystem::exists(good.temporary_path),
        "cleanup removed a resumable partial with extra bytes"
    );
    for (const auto& partial : {missing, short_file, invalid, negative}) {
        require(
            !fixture.has_record(partial) &&
                !std::filesystem::exists(partial.temporary_path),
            "cleanup left a safe invalid file or real database record"
        );
    }
    for (const auto& name : ordinary) {
        require(
            read_file(fixture.root / name) == "keep",
            "cleanup removed a lookalike ordinary file"
        );
    }
    require(
        read_file(protected_path) == "tracked" &&
            read_file(pending_path) == "pending" &&
            read_file(other_drive / orphan.filename()) == "other",
        "cleanup crossed a protected reference or Drive boundary"
    );
    const auto repeat = fixture.run({"--yes", "--output", "json"});
    require(
        repeat.exit_code == 0 &&
            repeat.standard_output.contains(R"("planned_files":"0")") &&
            repeat.standard_output.contains(R"("invalid_records":"0")"),
        "cleanup was not idempotent"
    );
}

class ConfirmationBackend final : public onedrive::cli::ConsoleBackend {
public:
    explicit ConfirmationBackend(std::function<void()> change)
        : change_{std::move(change)} {
    }
    void emit(const onedrive::cli::ConsoleEvent&) override {
    }
    bool confirm(const onedrive::cli::ConfirmationRequest&) override {
        change_();
        return true;
    }
    onedrive::cli::OutputMode output_mode() const noexcept override {
        return onedrive::cli::OutputMode::text;
    }
    onedrive::cli::UiMode ui_mode() const noexcept override {
        return onedrive::cli::UiMode::console;
    }

private:
    std::function<void()> change_;
};

void test_changed_file() {
    for (const auto action : {0, 1, 2, 3, 4, 5, 6}) {
        CleanupFixture fixture;
        const auto partial = fixture.partial(
            "changed.bin",
            8,
            9,
            action == 6 ? std::nullopt : std::optional<std::string>{"short"}
        );
        const onedrive::logging::Session logging{{.level = "off"}};
        const onedrive::cli::Console console{
            std::make_unique<ConfirmationBackend>([&] {
                if (action == 0) {
                    write_file(partial.temporary_path, "new contents");
                } else if (action == 1) {
                    std::filesystem::remove(partial.temporary_path);
                } else if (action == 2) {
                    std::filesystem::permissions(
                        fixture.root,
                        std::filesystem::perms::owner_read |
                            std::filesystem::perms::owner_exec
                    );
                } else if (action == 3) {
                    std::filesystem::rename(
                        partial.temporary_path, fixture.root / "old-file"
                    );
                    write_file(partial.temporary_path, "short");
                } else if (action == 4) {
                    write_file(partial.temporary_path, "other");
                } else if (action == 5) {
                    std::filesystem::permissions(
                        partial.temporary_path,
                        std::filesystem::perms::owner_read
                    );
                } else {
                    write_file(partial.temporary_path, "new");
                }
            })
        };
        const onedrive::app::RuntimeFactory runtime{
            onedrive::util::borrowed_proxy, fixture.cli.runtime_factory
        };
        const auto result = onedrive::app::detail::cleanup_state(
            onedrive::config::Config::load(fixture.cli.config_path),
            runtime,
            console,
            false,
            false
        );
        std::filesystem::permissions(
            fixture.root, std::filesystem::perms::owner_all
        );
        const bool failed =
            action == 0 || action >= 3 || (action == 2 && ::geteuid() != 0);
        require(
            result == (failed ? 1 : 0),
            "changed or inaccessible cleanup target returned the wrong status"
        );
        require(
            fixture.has_record(partial) == failed,
            "cleanup removed the record after file deletion failed"
        );
    }
}

void test_unsafe_files() {
    CleanupFixture fixture;
    const auto partial = fixture.partial("link.bin", 8, 9, std::nullopt);
    const auto outside = fixture.cli.temporary_directory.path() / "outside";
    write_file(outside, "outside");
    std::filesystem::create_symlink(outside, partial.temporary_path);
    const auto hardlink = fixture.root / ".hardlink.onedrive-partial-1-2";
    std::filesystem::create_hard_link(outside, hardlink);
    const auto directory = fixture.root / ".directory.onedrive-partial-1-2";
    std::filesystem::create_directory(directory);
    const auto orphan = fixture.root / ".safe.onedrive-partial-1-2";
    write_file(orphan, "orphan");
    const auto result = fixture.run({"--yes", "--output", "json"});
    require(
        result.exit_code == 1 &&
            result.standard_output.contains(
                R"("event":"cleanup_incomplete")"
            ) &&
            result.standard_output.contains(R"("skipped":"3")") &&
            !std::filesystem::exists(orphan) && fixture.has_record(partial) &&
            std::filesystem::is_symlink(partial.temporary_path) &&
            std::filesystem::exists(hardlink) &&
            std::filesystem::is_directory(directory) &&
            read_file(outside) == "outside",
        "unsafe files were removed or skips were hidden"
    );
    if (::geteuid() != 0) {
        const auto blocked = fixture.root / "unreadable";
        std::filesystem::create_directory(blocked);
        write_file(orphan, "orphan");
        std::filesystem::permissions(blocked, std::filesystem::perms::none);
        const auto incomplete = fixture.run({"--yes", "--output", "json"});
        std::filesystem::permissions(
            blocked, std::filesystem::perms::owner_all
        );
        require(
            incomplete.exit_code == 1 && !incomplete.standard_error.empty() &&
                std::filesystem::exists(orphan),
            "incomplete scan deleted files or reported success"
        );
    }
}

void test_references_and_missing_state() {
    CleanupFixture fixture;
    std::vector<std::filesystem::path> referenced;
    const auto path = [&](std::string name) {
        auto value = fixture.root / ("." + name + ".onedrive-partial-1-2");
        write_file(value, name);
        referenced.push_back(value);
        return value;
    };
    const auto duplicate = fixture.partial("duplicate.bin", 4, 8, "x");
    const auto pending_partial = fixture.partial("pending.bin", 4, 8, "x");
    fixture.database([&](auto& db) {
        auto alias = duplicate;
        alias.item.remote_id = "alias";
        db.save_partial_download(alias);
        db.save_pending_download({
            .item = pending_partial.item,
            .temporary_path = pending_partial.temporary_path,
            .content_fingerprint = "content",
            .backup_path = path("backup"),
            .backup_fingerprint = "backup",
        });
        db.save_pending_upload({
            .drive_id = fixture.identity.drive_id,
            .remote_path = "upload",
            .local_path = path("upload"),
            .snapshot_path = path("snapshot"),
            .content_fingerprint = "content",
        });
        db.save_pending_delete({
            .drive_id = fixture.identity.drive_id,
            .remote_id = "delete",
            .expected_etag = "etag",
            .remote_path = "delete",
            .local_path = path("delete"),
        });
        db.save_pending_move({
            .drive_id = fixture.identity.drive_id,
            .remote_id = "move",
            .source_path = path("source"),
            .destination_path = path("destination"),
            .staging_path = path("staging"),
            .source_device = 1,
            .source_inode = 1,
        });
        db.save_pending_remote_move({
            .drive_id = fixture.identity.drive_id,
            .remote_id = "remote-move",
            .expected_etag = "etag",
            .source_remote_path = "source",
            .destination_remote_path = "destination",
            .source_local_path = path("remote-source"),
            .destination_local_path = path("remote-destination"),
            .local_device = 1,
            .local_inode = 1,
        });
    });
    const auto result = fixture.run({"--yes", "--output", "json"});
    require(
        result.exit_code == 1 &&
            result.standard_output.contains(R"("skipped":"3")") &&
            result.standard_output.contains(R"("planned_files":"0")") &&
            fixture.has_record(duplicate) &&
            fixture.has_record(pending_partial),
        "cleanup did not preserve duplicate or pending partial references"
    );
    for (const auto& file : referenced) {
        require(
            std::filesystem::exists(file),
            "cleanup removed a file referenced by an unfinished operation"
        );
    }

    CleanupFixture missing_state;
    const auto unrecorded = missing_state.root / ".orphan.onedrive-partial-1-2";
    write_file(unrecorded, "orphan");
    const auto orphan_only = missing_state.run({"--yes", "--output", "json"});
    require(
        orphan_only.exit_code == 0 && !std::filesystem::exists(unrecorded) &&
            !std::filesystem::exists(
                missing_state.database_directory / "items.sqlite3"
            ),
        "orphan-only cleanup unnecessarily created a database"
    );
    std::filesystem::remove(missing_state.root);
    require(
        missing_state.run({"--yes", "--output", "json"}).exit_code == 0,
        "cleanup failed on an absent Drive root"
    );
    write_file(missing_state.root, "not a directory");
    const auto bad_root = missing_state.run({"--yes", "--output", "json"});
    require(
        bad_root.exit_code == 1 && !bad_root.standard_error.empty(),
        "cleanup accepted a non-directory Drive root"
    );
}

void test_invalid_database_and_retry() {
    CleanupFixture fixture;
    const auto partial = fixture.partial("retry.bin", 4, 8, "x");
    const auto database = fixture.database_directory / "items.sqlite3";
    require(
        onedrive::test::db::execute_schema(
            database,
            "CREATE TRIGGER cleanup_test_failure BEFORE DELETE ON "
            "partial_download "
            "BEGIN SELECT RAISE(FAIL, 'injected record removal failure'); END;"
        ),
        "cannot inject record removal failure"
    );
    const auto failed = fixture.run({"--yes", "--output", "json"});
    require(
        failed.exit_code == 1 && !failed.standard_error.empty() &&
            std::filesystem::exists(partial.temporary_path),
        "cleanup did not reject an unexpected database schema before deletion"
    );
    require(
        onedrive::test::db::execute_schema(
            database, "DROP TRIGGER cleanup_test_failure;"
        ),
        "cannot remove record failure injection"
    );
    require(
        fixture.has_record(partial), "database rejection lost a download record"
    );
    std::filesystem::remove(partial.temporary_path);
    const auto retry = fixture.run({"--yes", "--output", "json"});
    require(
        retry.exit_code == 0 && !fixture.has_record(partial) &&
            retry.standard_output.contains(R"("removed_records":"1")") &&
            retry.standard_output.contains(R"("removed_files":"0")"),
        "cleanup could not finish a record after its file was already deleted"
    );
}

} // namespace

int main() {
    try {
        test_cleanup();
        test_changed_file();
        test_unsafe_files();
        test_references_and_missing_state();
        test_invalid_database_and_retry();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}
