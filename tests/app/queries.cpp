#include "support.hpp"

#include "onedrive/app/queries.hpp"

#include <nlohmann/json.hpp>

namespace {

using namespace onedrive::test::app;

struct BlockedFixture {
    TemporaryDirectory temporary;
    onedrive::config::Config config{onedrive::config::Config::defaults()};
    onedrive::account::DriveIdentity identity{
        onedrive::test::test_drive_identity("me")
    };
    onedrive::account::AccountPaths paths;

    BlockedFixture() {
        config.state_directory = temporary.path() / "state";
        std::filesystem::create_directory(config.state_directory);
        paths = onedrive::account::AccountState::activate(
            config.state_directory, identity, "saved-test-token"
        );
    }

    void save_blocker() const {
        onedrive::storage::ItemDatabase database{
            paths.drive_directory, identity
        };
        database.open();
        onedrive::storage::BlockedItem item;
        item.drive_id = identity.drive_id;
        item.remote_id = "notes";
        item.remote_path = "Documents/notes.one";
        item.reason_code = "unsupported_onenote";
        item.reason_message = "Unsupported OneNote file";
        database.save_blocked_item(item);
        database.save_blocked_item(item);
    }
};

int test_blocked_query_absence_and_selection() {
    BlockedFixture fixture;
    auto absent = fixture.config;
    absent.state_directory = fixture.temporary.path() / "absent";
    if (!onedrive::app::load_blocked_items(absent).empty() ||
        std::filesystem::exists(absent.state_directory)) {
        return fail("offline blocker query created a missing account state");
    }
    const auto database = fixture.paths.drive_directory / "items.sqlite3";
    if (!onedrive::app::load_blocked_items(fixture.config).empty() ||
        std::filesystem::exists(database)) {
        return fail("offline blocker query created a missing item database");
    }
    auto unrelated = fixture.config;
    unrelated.drive_id = "unrelated-drive";
    if (!onedrive::app::load_blocked_items(unrelated).empty()) {
        return fail("offline blocker query selected an unrelated Drive");
    }
    fixture.save_blocker();
    onedrive::test::write_file(
        fixture.paths.drive_directory.parent_path() / "unrelated.txt", "ignore"
    );
    const auto before = onedrive::test::read_file(database);
    const auto modified = std::filesystem::last_write_time(database);
    for (const auto& drive :
         {fixture.identity.configured_drive_id, fixture.identity.drive_id}) {
        auto config = fixture.config;
        config.drive_id = drive;
        const auto blocked = onedrive::app::load_blocked_items(config);
        if (blocked.size() != 1 ||
            blocked[0].drive_id != fixture.identity.drive_id ||
            blocked[0].remote_id != "notes" || blocked[0].attempt_count != 2 ||
            blocked[0].reason_code != "unsupported_onenote") {
            return fail("configured/canonical Drive query lost saved blockers");
        }
    }
    if (onedrive::test::read_file(database) != before ||
        std::filesystem::last_write_time(database) != modified) {
        return fail("read-only blocker query changed the item database");
    }
    return EXIT_SUCCESS;
}

int test_blocked_query_metadata_errors() {
    for (const bool account : {false, true}) {
        BlockedFixture fixture;
        const auto metadata =
            account ? fixture.paths.account_directory / "account.json"
                    : fixture.paths.drive_directory / "drive.json";
        onedrive::test::write_file(metadata, "{ invalid json");
        if (!onedrive::test::throws_with<std::runtime_error>(
                [&] {
                    static_cast<void>(
                        onedrive::app::load_blocked_items(fixture.config)
                    );
                },
                "invalid account metadata"
            )) {
            return fail("malformed account/Drive JSON appeared as no blockers");
        }
    }
    for (const bool account : {false, true}) {
        BlockedFixture fixture;
        const auto metadata =
            account ? fixture.paths.account_directory / "account.json"
                    : fixture.paths.drive_directory / "drive.json";
        auto json = nlohmann::json::parse(onedrive::test::read_file(metadata));
        if (account) {
            json.erase("user_id");
        } else {
            json["configured_drive_id"] = 42;
        }
        onedrive::test::write_file(metadata, json.dump());
        if (!onedrive::test::throws_with<std::runtime_error>(
                [&] {
                    static_cast<void>(
                        onedrive::app::load_blocked_items(fixture.config)
                    );
                },
                "invalid saved account/drive metadata"
            )) {
            return fail("invalid account/Drive fields appeared as no blockers");
        }
    }
    BlockedFixture ambiguous;
    const auto duplicate =
        ambiguous.paths.drive_directory.parent_path() / "duplicate";
    std::filesystem::create_directory(duplicate);
    std::filesystem::copy_file(
        ambiguous.paths.drive_directory / "drive.json", duplicate / "drive.json"
    );
    if (!onedrive::test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(
                    onedrive::app::load_blocked_items(ambiguous.config)
                );
            },
            "multiple saved drives"
        )) {
        return fail("ambiguous saved Drives were silently selected");
    }
    return EXIT_SUCCESS;
}

int test_blocked_query_rejects_unsafe_state() {
    for (const bool database_link : {false, true}) {
        BlockedFixture fixture;
        if (database_link) {
            std::filesystem::create_symlink(
                fixture.temporary.path() / "outside.sqlite3",
                fixture.paths.drive_directory / "items.sqlite3"
            );
        } else {
            std::filesystem::create_directory_symlink(
                fixture.paths.drive_directory,
                fixture.paths.drive_directory.parent_path() / "linked-drive"
            );
        }
        if (!onedrive::test::throws_with<std::runtime_error>(
                [&] {
                    static_cast<void>(
                        onedrive::app::load_blocked_items(fixture.config)
                    );
                },
                "symbolic link"
            )) {
            return fail("symbolic-link state was accepted by blocker query");
        }
    }
    BlockedFixture readable;
    const auto metadata = readable.paths.drive_directory / "drive.json";
    std::filesystem::permissions(
        metadata,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::group_read
    );
    if (!onedrive::test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(
                    onedrive::app::load_blocked_items(readable.config)
                );
            },
            "readable and writable only by the current user"
        )) {
        return fail("non-private Drive metadata was accepted by blocker query");
    }
    BlockedFixture corrupt;
    onedrive::test::write_file(
        corrupt.paths.drive_directory / "items.sqlite3", "not a database"
    );
    std::filesystem::permissions(
        corrupt.paths.drive_directory / "items.sqlite3",
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
    );
    if (!onedrive::test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(
                    onedrive::app::load_blocked_items(corrupt.config)
                );
            },
            "database"
        )) {
        return fail("corrupt item database appeared as an empty blocker list");
    }
    return EXIT_SUCCESS;
}

int test_blocked_item_matching() {
    onedrive::storage::BlockedItem item;
    item.remote_path = "Documents/notes.one";
    item.reason_code = "future_reason";
    item.reason_message = "Saved diagnostic";
    for (const auto needle : {"Documents", "future_reason", "diagnostic", ""}) {
        const auto text_matches = [needle](const std::string& text) {
            return text.contains(needle);
        };
        if (!onedrive::app::matches_blocked_item(
                item, std::nullopt, text_matches
            ) ||
            !onedrive::app::matches_blocked_item(
                item, item.reason_code, text_matches
            )) {
            return fail("shared blocker filter omitted a searchable field");
        }
    }
    int calls = 0;
    const auto always_matches = [&](const std::string&) {
        ++calls;
        return true;
    };
    for (const auto reason : {"FUTURE_REASON", "future", ""}) {
        if (onedrive::app::matches_blocked_item(item, reason, always_matches)) {
            return fail(
                "shared blocker filter did not require an exact reason"
            );
        }
    }
    if (calls != 0 ||
        onedrive::app::matches_blocked_item(
            item, std::nullopt, [](const std::string&) { return false; }
        )) {
        return fail(
            "shared blocker filter ignored a mismatch or short circuit"
        );
    }
    return EXIT_SUCCESS;
}

int test_quota_snapshot() {
    FakeRuntimeFactory implementation;
    const onedrive::app::RuntimeFactory runtime_factory{
        onedrive::util::borrowed_proxy, implementation
    };
    const auto config = onedrive::config::Config::defaults();
    const auto snapshot =
        onedrive::app::load_quota_snapshot(config, runtime_factory);
    if (snapshot.drive.id != "drive-id" ||
        snapshot.drive.name != "Test Drive" ||
        snapshot.drive.type != "business" ||
        snapshot.drive.web_url != "https://example.test/drive" ||
        snapshot.drive.owner != "Test User" || !snapshot.drive.quota ||
        snapshot.drive.quota->total != 2'048 ||
        snapshot.drive.quota->used != 400 ||
        snapshot.drive.quota->remaining != 600 ||
        snapshot.drive.quota->deleted != 25 ||
        snapshot.drive.quota->state != "normal" ||
        implementation.graph_info_client_count != 1) {
        return fail("quota query did not return the structured drive data");
    }
    return EXIT_SUCCESS;
}

int test_status_snapshot() {
    TemporaryDirectory temporary;
    FakeRuntimeFactory implementation;
    const onedrive::app::RuntimeFactory runtime_factory{
        onedrive::util::borrowed_proxy, implementation
    };
    auto config = onedrive::config::Config::defaults();
    config.state_directory = temporary.path() / "state";
    config.sync_mode = onedrive::sync::SyncMode::upload_only;
    config.delete_policy = onedrive::sync::DeletePolicy::preserve;
    config.monitor_websocket_enabled = false;
    std::filesystem::create_directories(config.state_directory);

    const auto initial =
        onedrive::app::load_sync_status_snapshot(config, runtime_factory);
    if (initial.identity.user_display_name != "Test User" ||
        initial.identity.drive_id != "drive-id" ||
        initial.identity.drive_name != "Test Drive" ||
        initial.sync_mode != onedrive::sync::SyncMode::upload_only ||
        initial.delete_policy != onedrive::sync::DeletePolicy::preserve ||
        initial.last_run || initial.state.database_present ||
        initial.websocket_enabled ||
        implementation.graph_info_client_count != 1) {
        return fail("status query did not return the initial structured state");
    }

    const auto paths = onedrive::account::AccountState::locate(
        config.state_directory, initial.identity
    );
    std::filesystem::create_directories(paths.drive_directory);
    onedrive::metrics::FileMetrics{paths.drive_directory}.record_sync_run(
        onedrive::metrics::SyncRunOutcome::failed,
        std::chrono::milliseconds{250}
    );
    const auto failed =
        onedrive::app::load_sync_status_snapshot(config, runtime_factory);
    if (!failed.last_run ||
        failed.last_run->outcome != onedrive::metrics::SyncRunOutcome::failed ||
        failed.last_run->duration_milliseconds != 250 ||
        failed.last_run->completed_at_unix_seconds <= 0 ||
        implementation.graph_info_client_count != 2) {
        return fail("status query did not return the recorded sync result");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (test_blocked_item_matching() != EXIT_SUCCESS ||
        test_blocked_query_absence_and_selection() != EXIT_SUCCESS ||
        test_blocked_query_metadata_errors() != EXIT_SUCCESS ||
        test_blocked_query_rejects_unsafe_state() != EXIT_SUCCESS ||
        test_quota_snapshot() != EXIT_SUCCESS ||
        test_status_snapshot() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
