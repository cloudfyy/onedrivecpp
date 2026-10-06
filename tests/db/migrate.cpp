#include "support.hpp"
#include "onedrive/storage/item_database.hpp"
#include "test_support.hpp"

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
    struct LegacyMigrationFixture {
        const char* name;
        bool (*create)(const std::filesystem::path&);
    };
    constexpr LegacyMigrationFixture legacy_migrations[] = {
        {"version-one", create_version_one_database},
        {"version-two", create_version_two_database},
        {"version-three", create_version_three_database},
    };
    for (const auto& fixture : legacy_migrations) {
        const auto directory = temporary_directory.path() / fixture.name;
        const auto database_path = directory / "items.sqlite3";
        std::filesystem::create_directories(directory);
        if (!fixture.create(database_path)) {
            return fail(
                std::string{fixture.name} +
                " migration fixture could not be created"
            );
        }
        {
            onedrive::storage::ItemDatabase database{directory, identity()};
            database.open();
        }
        if (!schema_version_is(database_path, 24) ||
            !identity_row_is_valid(database_path)) {
            return fail(
                std::string{fixture.name} +
                " database was not migrated to the current schema"
            );
        }
    }

    const auto migration_directory =
        temporary_directory.path() / "version-four";
    std::filesystem::create_directories(migration_directory);
    if (!create_version_four_database(migration_directory / "items.sqlite3")) {
        return fail("version four migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            migration_directory, identity()
        };
        database.open();
        database.apply_delta({
            .drive_id = "me",
            .blocked_upserts =
                {
                    {
                        .remote_id = "migrated-blocked",
                        .name = "blocked.txt",
                        .etag = "etag",
                        .remote_path = "blocked.txt",
                        .reason_code = "local_modification",
                        .reason_message = "local file was modified",
                    },
                },
            .delta_link = "https://graph.example.test/migrated",
        });
        if (database.blocked_items("me").size() != 1) {
            return fail(
                "version four database was not migrated to blocked items"
            );
        }
    }
    {
        onedrive::storage::ItemDatabase database{
            migration_directory, identity()
        };
        database.open();
        if (database.blocked_items("me").size() != 1 ||
            database.blocked_items("me")[0].remote_id != "migrated-blocked") {
            return fail("migrated blocked item was not persisted");
        }
    }

    const auto version_five_directory =
        temporary_directory.path() / "version-five";
    std::filesystem::create_directories(version_five_directory);
    if (!create_version_five_database(
            version_five_directory / "items.sqlite3"
        )) {
        return fail("version five migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_five_directory, identity()
        };
        database.open();
        if (!identity_row_is_valid(version_five_directory / "items.sqlite3")) {
            return fail(
                "version five database did not gain account identity state"
            );
        }
    }

    const auto version_six_directory =
        temporary_directory.path() / "version-six";
    std::filesystem::create_directories(version_six_directory);
    if (!create_version_six_database(version_six_directory / "items.sqlite3")) {
        return fail("version six migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_six_directory, identity()
        };
        database.open();
    }
    if (!identity_row_is_valid(version_six_directory / "items.sqlite3")) {
        return fail("version six database did not gain the Drive ID mapping");
    }

    const auto version_seven_directory =
        temporary_directory.path() / "version-seven";
    std::filesystem::create_directories(version_seven_directory);
    if (!create_version_seven_database(
            version_seven_directory / "items.sqlite3"
        )) {
        return fail("version seven migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_seven_directory, identity()
        };
        database.open();
        database.save_partial_download({
            .item =
                {
                    .drive_id = "me",
                    .remote_id = "migrated-partial",
                    .name = "migrated-partial.txt",
                    .etag = "etag",
                    .remote_path = "migrated-partial.txt",
                    .local_path =
                        version_seven_directory / "migrated-partial.txt",
                    .size = 4,
                },
            .temporary_path = version_seven_directory / ".migrated-partial.tmp",
            .completed_bytes = 2,
        });
        if (!database.partial_download("me", "migrated-partial")) {
            return fail(
                "version seven database did not gain partial download state"
            );
        }
    }

    const auto version_eight_directory =
        temporary_directory.path() / "version-eight";
    std::filesystem::create_directories(version_eight_directory);
    if (!create_version_eight_database(
            version_eight_directory / "items.sqlite3"
        )) {
        return fail("version eight migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_eight_directory, identity()
        };
        database.open();
        database.apply_delta({
            .drive_id = "me",
            .blocked_upserts =
                {
                    {
                        .remote_id = "hash-after-migration",
                        .name = "hash.txt",
                        .etag = "etag",
                        .remote_path = "hash.txt",
                        .reason_code = "local_modification",
                        .reason_message = "local file was modified",
                        .content_hash =
                            onedrive::util::FileHash{
                                .algorithm =
                                    onedrive::util::FileHashAlgorithm::sha256,
                                .value = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                                         "AAAAAAAAAAA"
                                         "AAAAAAAAAAAAAAAA",
                            },
                    },
                },
            .delta_link = "https://graph.example.test/delta-v9",
            .sync_filter_fingerprint = "migrated-filter",
        });
        const auto blocked = database.blocked_items("me");
        if (blocked.size() != 1 || !blocked[0].content_hash ||
            blocked[0].content_hash->algorithm !=
                onedrive::util::FileHashAlgorithm::sha256 ||
            database.sync_filter_fingerprint("me") !=
                std::optional<std::string>{"migrated-filter"}) {
            return fail(
                "version eight database did not gain blocked hash metadata"
            );
        }
    }

    const auto version_nine_directory =
        temporary_directory.path() / "version-nine";
    std::filesystem::create_directories(version_nine_directory);
    if (!create_version_nine_database(
            version_nine_directory / "items.sqlite3"
        )) {
        return fail("version nine migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_nine_directory, identity()
        };
        database.open();
        database.apply_delta({
            .drive_id = "me",
            .delta_link = "https://graph.example.test/delta-v10",
            .sync_filter_fingerprint = "version-nine-filter",
        });
        if (database.sync_filter_fingerprint("me") !=
            std::optional<std::string>{"version-nine-filter"}) {
            return fail(
                "version nine database did not gain selective sync state"
            );
        }
    }

    const auto version_ten_directory =
        temporary_directory.path() / "version-ten";
    std::filesystem::create_directories(version_ten_directory);
    if (!create_version_ten_database(version_ten_directory / "items.sqlite3")) {
        return fail("version ten migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_ten_directory, identity()
        };
        database.open();
        database.save_pending_download({
            .item =
                {
                    .drive_id = "me",
                    .remote_id = "safe-backup-after-migration",
                    .name = "report.txt",
                    .etag = "etag",
                    .remote_path = "report.txt",
                    .local_path = version_ten_directory / "report.txt",
                    .last_modified = "2026-10-04T05:10:00Z",
                    .size = 4,
                },
            .temporary_path = version_ten_directory / ".report.partial",
            .content_fingerprint = "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602"
                                   "261a9a923d3bb23adc8b7",
            .backup_path = version_ten_directory /
                           "report.safeBackup-20261004T051000Z-0001.txt",
            .backup_fingerprint = "ca3704aa0b06f5954c79ee837faa152d84c3fb2ceca2"
                                  "ba352a4a014fab6e5e2c",
        });
        const auto pending = database.pending_downloads("me");
        if (pending.size() != 1 ||
            pending[0].backup_path.filename() !=
                "report.safeBackup-20261004T051000Z-0001.txt" ||
            pending[0].backup_fingerprint !=
                "ca3704aa0b06f5954c79ee837faa152d84c3fb2ceca2ba352a4a014fab6e5e"
                "2c") {
            return fail(
                "version ten database did not gain safeBackup journal state"
            );
        }
    }

    const auto version_eleven_directory =
        temporary_directory.path() / "version-eleven";
    std::filesystem::create_directories(version_eleven_directory);
    if (!create_version_eleven_database(
            version_eleven_directory / "items.sqlite3"
        )) {
        return fail("version eleven migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_eleven_directory, identity()
        };
        database.open();
        database.apply_delta({
            .drive_id = "me",
            .blocked_upserts =
                {
                    {
                        .remote_id = "deleted-after-migration",
                        .name = "deleted.txt",
                        .remote_path = "deleted.txt",
                        .deleted = true,
                        .reason_code = "local_modification",
                        .reason_message = "local file changed",
                    },
                },
            .delta_link = "https://graph.example.test/delta-v12",
        });
        const auto blocked = database.blocked_items("me");
        if (blocked.size() != 1 || !blocked[0].deleted) {
            return fail(
                "version eleven database did not gain deletion retry state"
            );
        }
    }

    const auto version_twelve_directory =
        temporary_directory.path() / "version-twelve";
    std::filesystem::create_directories(version_twelve_directory);
    if (!create_version_twelve_database(
            version_twelve_directory / "items.sqlite3"
        )) {
        return fail("version twelve migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_twelve_directory, identity()
        };
        database.open();
        const onedrive::storage::PendingUpload pending{
            .drive_id = "me",
            .remote_path = "upload.txt",
            .local_path = version_twelve_directory / "upload.txt",
            .snapshot_path =
                version_twelve_directory / ".upload.onedrive-upload-1",
            .content_fingerprint = "239f59ed55e737c77147cf55ad0c1b030b6d7ee748a"
                                   "7426952f9b852d5a935e5",
            .local_size = 7,
            .local_modified_ticks = 123,
        };
        database.save_pending_upload(pending);
        const auto uploads = database.pending_uploads("me");
        if (uploads.size() != 1 || uploads[0].remote_path != "upload.txt" ||
            uploads[0].local_size != 7) {
            return fail(
                "version twelve database did not gain pending upload state"
            );
        }
        database.commit_upload(
            pending,
            {
                .drive_id = "me",
                .remote_id = "uploaded-id",
                .name = "upload.txt",
                .etag = "uploaded-etag",
                .remote_path = "upload.txt",
                .local_path = pending.local_path,
                .last_modified = "2026-10-04T09:00:00Z",
                .size = 7,
                .local_size = 7,
                .local_modified_ticks = 123,
            }
        );
        if (!database.pending_uploads("me").empty() ||
            !database.find("me", "uploaded-id")) {
            return fail("pending upload commit was not atomic");
        }
    }

    const auto version_fifteen_directory =
        temporary_directory.path() / "version-fifteen";
    std::filesystem::create_directories(version_fifteen_directory);
    if (!create_version_fifteen_database(
            version_fifteen_directory / "items.sqlite3"
        )) {
        return fail("version fifteen migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_fifteen_directory, identity()
        };
        database.open();
        database.apply_delta({
            .drive_id = "me",
            .upload_suppressions =
                {
                    {
                        .remote_id = "migrated-suppression",
                        .local_path =
                            version_fifteen_directory / "retained.txt",
                        .source_device = 111,
                        .source_inode = 222,
                    },
                },
            .delta_link = "https://graph.example.test/v16",
        });
        const auto suppressions = database.upload_suppressions("me");
        if (suppressions.size() != 1 ||
            suppressions[0].remote_id != "migrated-suppression" ||
            suppressions[0].source_device != 111 ||
            suppressions[0].source_inode != 222) {
            return fail(
                "version fifteen database did not gain upload suppressions"
            );
        }
    }

    const auto version_sixteen_directory =
        temporary_directory.path() / "version-sixteen";
    std::filesystem::create_directories(version_sixteen_directory);
    if (!create_version_sixteen_database(
            version_sixteen_directory / "items.sqlite3"
        )) {
        return fail("version sixteen migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_sixteen_directory, identity()
        };
        database.open();
        const onedrive::storage::PendingUpload pending{
            .drive_id = "me",
            .remote_path = "large.bin",
            .local_path = version_sixteen_directory / "large.bin",
            .snapshot_path =
                version_sixteen_directory / ".large.onedrive-upload-1",
            .content_fingerprint = "239f59ed55e737c77147cf55ad0c1b030b6d7ee748a"
                                   "7426952f9b852d5a935e5",
            .local_size = 655360,
            .local_modified_ticks = 456,
            .upload_url = "https://upload.example.test/session?secret=1",
            .upload_expiration = "2099-10-05T09:00:00Z",
            .completed_bytes = 327680,
        };
        database.save_pending_upload(pending);
        const auto uploads = database.pending_uploads("me");
        if (uploads.size() != 1 ||
            uploads[0].upload_url != pending.upload_url ||
            uploads[0].upload_expiration != pending.upload_expiration ||
            uploads[0].completed_bytes != pending.completed_bytes) {
            return fail(
                "version sixteen database did not persist upload checkpoints"
            );
        }
    }

    const auto version_seventeen_directory =
        temporary_directory.path() / "version-seventeen";
    std::filesystem::create_directories(version_seventeen_directory);
    if (!create_version_seventeen_database(
            version_seventeen_directory / "items.sqlite3"
        )) {
        return fail("version seventeen migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_seventeen_directory, identity()
        };
        database.open();
        const onedrive::storage::PendingUpload pending{
            .drive_id = "me",
            .remote_path = "Parent/Child",
            .local_path = version_seventeen_directory / "Parent" / "Child",
            .snapshot_path = {},
            .content_fingerprint = {},
            .local_size = 0,
            .local_modified_ticks = 0,
            .remote_id = std::nullopt,
            .expected_etag = {},
            .upload_url = {},
            .upload_expiration = {},
            .completed_bytes = 0,
            .directory = true,
        };
        database.save_pending_upload(pending);
        const auto uploads = database.pending_uploads("me");
        if (uploads.size() != 1 || !uploads[0].directory ||
            uploads[0].remote_path != pending.remote_path) {
            return fail(
                "version seventeen database did not persist directory uploads"
            );
        }
        database.remove_pending_upload("me", pending.remote_path);
        if (!database.pending_uploads("me").empty()) {
            return fail("pending directory upload was not removed");
        }
    }

    const auto version_eighteen_directory =
        temporary_directory.path() / "version-eighteen";
    std::filesystem::create_directories(version_eighteen_directory);
    if (!create_version_eighteen_database(
            version_eighteen_directory / "items.sqlite3"
        )) {
        return fail("version eighteen migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_eighteen_directory, identity()
        };
        database.open();
        database.upsert({
            .drive_id = "me",
            .remote_id = "deleted-directory",
            .name = "Deleted",
            .etag = "delete-etag",
            .remote_path = "Deleted",
            .local_path = version_eighteen_directory / "Deleted",
            .directory = true,
        });
        database.upsert({
            .drive_id = "me",
            .remote_id = "deleted-child",
            .parent_id = "deleted-directory",
            .name = "child.txt",
            .etag = "child-etag",
            .remote_path = "Deleted/child.txt",
            .local_path = version_eighteen_directory / "Deleted" / "child.txt",
        });
        const onedrive::storage::PendingDelete deletion{
            .drive_id = "me",
            .remote_id = "deleted-directory",
            .expected_etag = "delete-etag",
            .remote_path = "Deleted",
            .local_path = version_eighteen_directory / "Deleted",
            .directory = true,
        };
        database.save_pending_delete(deletion);
        const auto deletions = database.pending_deletes("me");
        if (deletions.size() != 1 ||
            deletions[0].expected_etag != "delete-etag") {
            return fail(
                "version eighteen database did not gain deletion journal"
            );
        }
        database.commit_delete(deletion);
        if (!database.pending_deletes("me").empty() ||
            database.find("me", "deleted-directory") ||
            database.find("me", "deleted-child")) {
            return fail("pending directory deletion commit was not atomic");
        }
    }

    const auto version_nineteen_directory =
        temporary_directory.path() / "version-nineteen";
    std::filesystem::create_directories(version_nineteen_directory);
    if (!create_version_nineteen_database(
            version_nineteen_directory / "items.sqlite3"
        )) {
        return fail("version nineteen migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_nineteen_directory, identity()
        };
        database.open();
        database.upsert({
            .drive_id = "me",
            .remote_id = "identity-item",
            .name = "identity.txt",
            .etag = "identity-etag",
            .remote_path = "identity.txt",
            .local_path = version_nineteen_directory / "identity.txt",
            .local_device = 123,
            .local_inode = 456,
        });
        const auto item = database.find("me", "identity-item");
        if (!item || item->local_device != 123 || item->local_inode != 456) {
            return fail(
                "version nineteen database did not persist local identity"
            );
        }
    }

    const auto version_twenty_directory =
        temporary_directory.path() / "version-twenty";
    std::filesystem::create_directories(version_twenty_directory);
    if (!create_version_twenty_database(
            version_twenty_directory / "items.sqlite3"
        )) {
        return fail("version twenty migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_twenty_directory, identity()
        };
        database.open();
        database.upsert({
            .drive_id = "me",
            .remote_id = "move-directory",
            .name = "Old",
            .etag = "old-etag",
            .remote_path = "Old",
            .local_path = version_twenty_directory / "Old",
            .local_device = 11,
            .local_inode = 22,
            .directory = true,
        });
        database.upsert({
            .drive_id = "me",
            .remote_id = "move-child",
            .parent_id = "move-directory",
            .name = "child.txt",
            .etag = "child-etag",
            .remote_path = "Old/child.txt",
            .local_path = version_twenty_directory / "Old" / "child.txt",
            .local_device = 11,
            .local_inode = 23,
        });
        const onedrive::storage::PendingRemoteMove move{
            .drive_id = "me",
            .remote_id = "move-directory",
            .expected_etag = "old-etag",
            .source_remote_path = "Old",
            .destination_remote_path = "New",
            .source_local_path = version_twenty_directory / "Old",
            .destination_local_path = version_twenty_directory / "New",
            .local_device = 11,
            .local_inode = 22,
            .directory = true,
        };
        database.save_pending_remote_move(move);
        if (database.pending_remote_moves("me").size() != 1) {
            return fail(
                "version twenty database did not gain remote move journal"
            );
        }
        database.commit_remote_move(
            move,
            {
                .drive_id = "me",
                .remote_id = "move-directory",
                .name = "New",
                .etag = "new-etag",
                .remote_path = "New",
                .local_path = version_twenty_directory / "New",
                .local_device = 11,
                .local_inode = 22,
                .directory = true,
            }
        );
        const auto child = database.find("me", "move-child");
        if (!database.pending_remote_moves("me").empty() || !child ||
            child->remote_path != "New/child.txt" ||
            child->local_path !=
                version_twenty_directory / "New" / "child.txt") {
            return fail("remote directory move commit was not atomic");
        }
    }

    const auto version_twenty_one_directory =
        temporary_directory.path() / "version-twenty-one";
    std::filesystem::create_directories(version_twenty_one_directory);
    if (!create_version_twenty_one_database(
            version_twenty_one_directory / "items.sqlite3"
        )) {
        return fail(
            "version twenty-one migration fixture could not be created"
        );
    }
    {
        onedrive::storage::ItemDatabase database{
            version_twenty_one_directory, identity()
        };
        database.open();
        database.upsert({
            .drive_id = "me",
            .remote_id = "ctag-item",
            .name = "ctag.txt",
            .etag = "metadata-version",
            .ctag = "content-version",
            .remote_path = "ctag.txt",
            .local_path = version_twenty_one_directory / "ctag.txt",
        });
        const auto item = database.find("me", "ctag-item");
        if (!item || item->ctag != "content-version") {
            return fail(
                "version twenty-one database did not persist content tags"
            );
        }
    }

    const auto version_twenty_two_directory =
        temporary_directory.path() / "version-twenty-two";
    std::filesystem::create_directories(version_twenty_two_directory);
    if (!create_version_twenty_two_database(
            version_twenty_two_directory / "items.sqlite3"
        )) {
        return fail(
            "version twenty-two migration fixture could not be created"
        );
    }
    {
        onedrive::storage::ItemDatabase database{
            version_twenty_two_directory, identity()
        };
        database.open();
        const onedrive::storage::PendingUpload failure{
            .drive_id = "me",
            .remote_path = "quota.txt",
            .local_path = version_twenty_two_directory / "quota.txt",
            .snapshot_path = version_twenty_two_directory / ".quota.upload",
            .content_fingerprint = "fingerprint",
            .local_size = 5,
            .failure_code = "remote_quota",
            .failure_message = "OneDrive quota exceeded",
            .failure_attempt_count = 2,
        };
        database.save_pending_upload(failure);
        const auto uploads = database.pending_uploads("me");
        if (uploads.size() != 1 ||
            uploads[0].failure_code != failure.failure_code ||
            uploads[0].failure_message != failure.failure_message ||
            uploads[0].failure_attempt_count != 2) {
            return fail(
                "version twenty-two database did not persist upload failures"
            );
        }
    }

    const auto version_twenty_three_directory =
        temporary_directory.path() / "version-twenty-three";
    const auto version_twenty_three_path =
        version_twenty_three_directory / "items.sqlite3";
    std::filesystem::create_directories(version_twenty_three_directory);
    if (!create_version_twenty_three_database(version_twenty_three_path) ||
        !execute_schema(
            version_twenty_three_path,
            "INSERT INTO blocked_item ("
            "drive_id, remote_id, parent_id, name, etag, remote_path, "
            "last_modified, size, directory, deleted, reason_code, "
            "reason_message"
            ") VALUES ("
            "'me', 'blocked-ctag', 'root', 'blocked.txt', 'etag', "
            "'blocked.txt', '2026-10-05T00:00:00Z', 4, 0, 0, "
            "'local_modification', 'local file changed'"
            ");"
        )) {
        return fail(
            "version twenty-three migration fixture could not be created"
        );
    }
    {
        onedrive::storage::ItemDatabase database{
            version_twenty_three_directory, identity()
        };
        database.open();
        const auto migrated = database.blocked_items("me");
        if (migrated.size() != 1 || !migrated[0].ctag.empty()) {
            return fail(
                "version twenty-three blocked item ctag was not migrated"
            );
        }
        database.apply_delta({
            .drive_id = "me",
            .blocked_upserts =
                {
                    {
                        .remote_id = "blocked-ctag",
                        .name = "blocked.txt",
                        .etag = "metadata-version",
                        .ctag = "content-version",
                        .remote_path = "blocked.txt",
                        .reason_code = "local_modification",
                        .reason_message = "local file changed",
                    },
                },
            .delta_link = "https://graph.example.test/blocked-ctag",
        });
        const auto updated = database.blocked_items("me");
        if (updated.size() != 1 || updated[0].ctag != "content-version") {
            return fail("blocked item content tag was not persisted");
        }
    }
    if (!schema_version_is(version_twenty_three_path, 24)) {
        return fail(
            "version twenty-three database was not migrated to version 24"
        );
    }

    const auto version_fourteen_directory =
        temporary_directory.path() / "version-fourteen";
    std::filesystem::create_directories(version_fourteen_directory);
    if (!create_version_fourteen_database(
            version_fourteen_directory / "items.sqlite3"
        )) {
        return fail("version fourteen migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_fourteen_directory, identity()
        };
        database.open();
        auto moves = database.pending_moves("me");
        if (moves.size() != 1 || moves[0].remote_id != "legacy-move" ||
            !moves[0].staging_path.empty()) {
            return fail(
                "version fourteen pending move did not migrate to staging"
            );
        }
        const onedrive::storage::PendingMove staged{
            .drive_id = "me",
            .remote_id = "staged-move",
            .source_path = version_fourteen_directory / "A.txt",
            .destination_path = version_fourteen_directory / "B.txt",
            .staging_path =
                version_fourteen_directory / ".A.txt.onedrive-move-test",
            .source_device = 789,
            .source_inode = 987,
        };
        database.save_pending_move(staged);
        moves = database.pending_moves("me");
        const auto saved = std::ranges::find(
            moves, "staged-move", &onedrive::storage::PendingMove::remote_id
        );
        if (saved == moves.end() ||
            saved->staging_path != staged.staging_path ||
            saved->source_device != staged.source_device ||
            saved->source_inode != staged.source_inode) {
            return fail("staged pending move did not round trip");
        }
    }

    const auto version_thirteen_directory =
        temporary_directory.path() / "version-thirteen";
    std::filesystem::create_directories(version_thirteen_directory);
    if (!create_version_thirteen_database(
            version_thirteen_directory / "items.sqlite3"
        )) {
        return fail("version thirteen migration fixture could not be created");
    }
    {
        onedrive::storage::ItemDatabase database{
            version_thirteen_directory, identity()
        };
        database.open();
        const onedrive::storage::PendingMove pending{
            .drive_id = "me",
            .remote_id = "moved-id",
            .source_path = version_thirteen_directory / "old.txt",
            .destination_path = version_thirteen_directory / "new.txt",
            .source_device = 123,
            .source_inode = 456,
        };
        database.save_pending_move(pending);
        const auto moves = database.pending_moves("me");
        if (moves.size() != 1 || moves[0].source_path != pending.source_path ||
            moves[0].destination_path != pending.destination_path ||
            moves[0].source_device != 123 || moves[0].source_inode != 456) {
            return fail(
                "version thirteen database did not gain pending move state"
            );
        }
        try {
            database.apply_delta({
                .drive_id = "me",
                .upserts =
                    {
                        {
                            .remote_id = "moved-id",
                            .name = "new.txt",
                            .remote_path = "new.txt",
                            .local_path = pending.destination_path,
                        },
                    },
                .blocked_upserts =
                    {
                        {
                            .remote_id = "invalid-blocked",
                        },
                    },
                .delta_link = "https://graph.example.test/rollback",
            });
            return fail("invalid delta did not roll back pending move commit");
        } catch (const std::invalid_argument&) {
        }
        if (database.pending_moves("me").size() != 1 ||
            database.find("me", "moved-id")) {
            return fail("pending move was not restored by delta rollback");
        }
        database.apply_delta({
            .drive_id = "me",
            .upserts =
                {
                    {
                        .remote_id = "moved-id",
                        .name = "new.txt",
                        .remote_path = "new.txt",
                        .local_path = pending.destination_path,
                    },
                },
            .delta_link = "https://graph.example.test/moved",
        });
        if (!database.pending_moves("me").empty() ||
            !database.find("me", "moved-id")) {
            return fail("pending move commit was not atomic");
        }
    }

    return EXIT_SUCCESS;
}
