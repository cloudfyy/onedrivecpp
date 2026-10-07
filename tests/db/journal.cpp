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
    {
        onedrive::storage::ItemDatabase database{
            temporary_directory.path(), identity()
        };
        database.open();
        database.upsert({
            .remote_id = "remote-1",
            .etag = "etag-1",
            .local_path = "documents/report.txt",
        });
        database.upsert({
            .remote_id = "remote-2",
            .etag = "etag-2",
            .local_path = "photos/image.jpg",
        });
        database.apply_delta({
            .drive_id = "me",
            .delta_link = "https://graph.example.test/delta-2",
            .sync_filter_fingerprint = "filter-1",
        });
    }
    {
        onedrive::storage::ItemDatabase database{
            temporary_directory.path(), identity()
        };
        database.open();
        if (database.size() != 2 || database.find("me", "remote-3") ||
            database.delta_link("me") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-2"
                }) {
            return fail("updated delta state was not loaded");
        }

        database.apply_delta({
            .drive_id = "me",
            .upserts =
                {
                    {
                        .remote_id = "reset-me",
                        .name = "reset-me.txt",
                        .etag = "reset-me-etag",
                        .remote_path = "reset-me.txt",
                        .local_path =
                            temporary_directory.path() / "reset-me.txt",
                    },
                },
            .blocked_upserts =
                {
                    {
                        .remote_id = "blocked-me",
                        .name = "blocked-me.txt",
                        .etag = "blocked-etag",
                        .ctag = "blocked-ctag",
                        .remote_path = "blocked-me.txt",
                        .deleted = true,
                        .reason_code = "local_modification",
                        .reason_message = "local file was modified",
                        .content_hash =
                            onedrive::util::FileHash{
                                .algorithm = onedrive::util::FileHashAlgorithm::
                                    quick_xor,
                                .value = "SgAAAAAAAAAAAAAAAQAAAAAAAAA=",
                            },
                    },
                },
            .delta_link = "https://graph.example.test/delta-me",
        });
        database.apply_delta({
            .drive_id = "other-drive",
            .upserts =
                {
                    {
                        .remote_id = "keep-me",
                        .name = "keep-me.txt",
                        .etag = "keep-me-etag",
                        .remote_path = "keep-me.txt",
                        .local_path =
                            temporary_directory.path() / "keep-me.txt",
                    },
                },
            .blocked_upserts =
                {
                    {
                        .remote_id = "blocked-other",
                        .name = "blocked-other.txt",
                        .etag = "blocked-etag",
                        .remote_path = "blocked-other.txt",
                        .reason_code = "invalid_remote_path",
                        .reason_message = "invalid name",
                    },
                },
            .delta_link = "https://graph.example.test/delta-other",
        });
        database.save_pending_download({
            .item =
                {
                    .drive_id = "me",
                    .remote_id = "pending-me",
                    .name = "pending-me.txt",
                    .etag = "pending-etag",
                    .ctag = "pending-ctag",
                    .remote_path = "pending-me.txt",
                    .local_path = temporary_directory.path() / "pending-me.txt",
                    .size = 4,
                },
            .temporary_path = temporary_directory.path() / "pending-me.tmp",
            .content_fingerprint = "fingerprint-me",
            .backup_path = temporary_directory.path() /
                           "pending-me.safeBackup-20261004T051000Z-0001.txt",
            .backup_fingerprint = "backup-fingerprint-me",
        });
        database.save_pending_download({
            .item =
                {
                    .drive_id = "other-drive",
                    .remote_id = "pending-other",
                    .name = "pending-other.txt",
                    .etag = "pending-etag",
                    .remote_path = "pending-other.txt",
                    .local_path =
                        temporary_directory.path() / "pending-other.txt",
                    .size = 5,
                },
            .temporary_path = temporary_directory.path() / "pending-other.tmp",
            .content_fingerprint = "fingerprint-other",
        });
        database.save_partial_download({
            .item =
                {
                    .drive_id = "me",
                    .remote_id = "partial-me",
                    .name = "partial-me.txt",
                    .etag = "partial-etag",
                    .ctag = "partial-ctag",
                    .remote_path = "partial-me.txt",
                    .local_path = temporary_directory.path() / "partial-me.txt",
                    .size = 4,
                },
            .temporary_path = temporary_directory.path() / ".partial-me.tmp",
            .completed_bytes = 2,
        });
        database.save_partial_download({
            .item =
                {
                    .drive_id = "other-drive",
                    .remote_id = "partial-other",
                    .name = "partial-other.txt",
                    .etag = "partial-etag",
                    .remote_path = "partial-other.txt",
                    .local_path =
                        temporary_directory.path() / "partial-other.txt",
                    .size = 5,
                },
            .temporary_path = temporary_directory.path() / ".partial-other.tmp",
            .completed_bytes = 3,
        });
        database.save_pending_move({
            .drive_id = "me",
            .remote_id = "move-me",
            .source_path = temporary_directory.path() / "old-me.txt",
            .destination_path = temporary_directory.path() / "new-me.txt",
            .source_device = 10,
            .source_inode = 20,
        });
        database.save_pending_move({
            .drive_id = "other-drive",
            .remote_id = "move-other",
            .source_path = temporary_directory.path() / "old-other.txt",
            .destination_path = temporary_directory.path() / "new-other.txt",
            .source_device = 30,
            .source_inode = 40,
        });
        database.apply_delta({
            .drive_id = "me",
            .upload_suppressions =
                {
                    {
                        .remote_id = "suppressed-me",
                        .local_path =
                            temporary_directory.path() / "suppressed-me.txt",
                        .source_device = 50,
                        .source_inode = 60,
                    },
                },
            .delta_link = "https://graph.example.test/delta-me",
        });
        database.apply_delta({
            .drive_id = "other-drive",
            .upload_suppressions =
                {
                    {
                        .remote_id = "suppressed-other",
                        .local_path =
                            temporary_directory.path() / "suppressed-other.txt",
                        .source_device = 70,
                        .source_inode = 80,
                    },
                },
            .delta_link = "https://graph.example.test/delta-other",
        });
        const auto pending_me = database.pending_downloads("me");
        const auto partial_me = database.partial_download("me", "partial-me");
        if (pending_me.size() != 1 ||
            database.pending_downloads("other-drive").size() != 1 ||
            pending_me[0].item.ctag != "pending-ctag" ||
            pending_me[0].backup_path !=
                temporary_directory.path() /
                    "pending-me.safeBackup-20261004T051000Z-0001.txt" ||
            pending_me[0].backup_fingerprint != "backup-fingerprint-me" ||
            !partial_me || partial_me->item.ctag != "partial-ctag" ||
            !database.partial_download("other-drive", "partial-other") ||
            database.pending_moves("me").size() != 1 ||
            database.pending_moves("other-drive").size() != 1 ||
            database.upload_suppressions("me").size() != 1 ||
            database.upload_suppressions("other-drive").size() != 1 ||
            database.blocked_items("me").size() != 1 ||
            database.blocked_items("other-drive").size() != 1 ||
            database.blocked_items("me")[0].attempt_count != 1 ||
            database.blocked_items("me")[0].ctag != "blocked-ctag" ||
            !database.blocked_items("me")[0].deleted ||
            !database.blocked_items("me")[0].content_hash ||
            database.blocked_items("me")[0].content_hash->value !=
                "SgAAAAAAAAAAAAAAAQAAAAAAAAA=") {
            return fail(
                "pending downloads or blocked items were not saved by drive"
            );
        }
        try {
            database.save_pending_download({
                .item =
                    {
                        .drive_id = "me",
                        .remote_id = "invalid-backup-journal",
                    },
                .temporary_path = temporary_directory.path() / "invalid.tmp",
                .content_fingerprint = "fingerprint",
                .backup_path =
                    temporary_directory.path() / "invalid.safeBackup",
            });
            return fail("incomplete safeBackup journal metadata was accepted");
        } catch (const std::invalid_argument&) {
        }

        if (!database.reset("me") || database.size() != 4 ||
            !database.find("me", "reset-me") ||
            !database.find("other-drive", "keep-me") ||
            database.delta_link("me").has_value() ||
            database.pending_downloads("me").size() != 1 ||
            database.pending_downloads("other-drive").size() != 1 ||
            !database.partial_download("me", "partial-me") ||
            !database.partial_download("other-drive", "partial-other") ||
            database.pending_moves("me").size() != 1 ||
            database.pending_moves("other-drive").size() != 1 ||
            database.upload_suppressions("me").size() != 1 ||
            database.upload_suppressions("other-drive").size() != 1 ||
            database.blocked_items("me").size() != 1 ||
            database.blocked_items("other-drive").size() != 1 ||
            database.delta_link("other-drive") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-other"
                }) {
            return fail("cursor reset did not preserve recovery state");
        }
        if (database.reset("me")) {
            return fail("resetting an absent cursor reported a removal");
        }
    }

    {
        onedrive::storage::ItemDatabase database{
            temporary_directory.path(), identity()
        };
        database.open();
        if (database.size() != 4 || !database.find("me", "reset-me") ||
            !database.find("other-drive", "keep-me") ||
            database.delta_link("me").has_value() ||
            database.pending_downloads("me").size() != 1 ||
            database.pending_downloads("other-drive").size() != 1 ||
            !database.partial_download("me", "partial-me") ||
            !database.partial_download("other-drive", "partial-other") ||
            database.pending_moves("me").size() != 1 ||
            database.pending_moves("other-drive").size() != 1 ||
            database.upload_suppressions("me").size() != 1 ||
            database.upload_suppressions("other-drive").size() != 1 ||
            database.blocked_items("me").size() != 1 ||
            database.blocked_items("other-drive").size() != 1 ||
            database.delta_link("other-drive") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-other"
                }) {
            return fail("safe cursor reset was not persisted");
        }

        database.apply_delta({
            .drive_id = "me",
            .upserts =
                {
                    {
                        .remote_id = "fresh-me",
                        .name = "fresh-me.txt",
                        .etag = "fresh-me-etag",
                        .remote_path = "fresh-me.txt",
                        .local_path =
                            temporary_directory.path() / "fresh-me.txt",
                    },
                },
            .blocked_upserts =
                {
                    {
                        .remote_id = "fresh-blocked-me",
                        .name = "fresh-blocked-me.txt",
                        .etag = "fresh-blocked-etag",
                        .remote_path = "fresh-blocked-me.txt",
                        .reason_code = "local_modification",
                        .reason_message = "still modified",
                    },
                },
            .delta_link = "https://graph.example.test/delta-fresh",
            .apply_mode = onedrive::storage::DeltaApplyMode::replace,
        });
        if (database.size() != 4 || database.find("me", "reset-me") ||
            !database.find("me", "fresh-me") ||
            !database.find("other-drive", "keep-me") ||
            !database.find("", "remote-1") || !database.find("", "remote-2") ||
            database.pending_downloads("me").size() != 1 ||
            database.pending_downloads("other-drive").size() != 1 ||
            !database.partial_download("me", "partial-me") ||
            !database.partial_download("other-drive", "partial-other") ||
            database.pending_moves("me").size() != 1 ||
            database.pending_moves("other-drive").size() != 1 ||
            database.upload_suppressions("me").size() != 1 ||
            database.upload_suppressions("other-drive").size() != 1 ||
            database.blocked_items("me").size() != 1 ||
            database.blocked_items("me")[0].remote_id != "fresh-blocked-me" ||
            database.blocked_items("other-drive").size() != 1) {
            return fail(
                "initial delta did not replace only the selected drive"
            );
        }

        database.save_partial_download({
            .item =
                {
                    .drive_id = "cleanup-drive",
                    .remote_id = "remove-partial",
                    .name = "remove-partial.txt",
                    .etag = "remove-etag",
                    .remote_path = "remove-partial.txt",
                    .local_path =
                        temporary_directory.path() / "remove-partial.txt",
                    .size = 4,
                },
            .temporary_path =
                temporary_directory.path() / ".remove-partial.tmp",
            .completed_bytes = 2,
        });
        database.save_partial_download({
            .item =
                {
                    .drive_id = "cleanup-drive",
                    .remote_id = "keep-partial",
                    .name = "keep-partial.txt",
                    .etag = "keep-etag",
                    .remote_path = "keep-partial.txt",
                    .local_path =
                        temporary_directory.path() / "keep-partial.txt",
                    .size = 4,
                },
            .temporary_path = temporary_directory.path() / ".keep-partial.tmp",
            .completed_bytes = 2,
        });
        const auto cleanup_partials =
            database.partial_downloads("cleanup-drive");
        if (cleanup_partials.size() != 2 ||
            cleanup_partials[0].item.remote_id != "keep-partial" ||
            cleanup_partials[1].item.remote_id != "remove-partial") {
            return fail("partial download listing was not ordered by path");
        }
        database.apply_delta({
            .drive_id = "cleanup-drive",
            .partial_download_removals = {"remove-partial"},
            .delta_link = "https://graph.example.test/delta-cleanup",
        });
        if (database.partial_download("cleanup-drive", "remove-partial") ||
            !database.partial_download("cleanup-drive", "keep-partial") ||
            database.partial_downloads("cleanup-drive").size() != 1) {
            return fail("delta did not remove only the selected partial");
        }
        try {
            database.apply_delta({
                .drive_id = "cleanup-drive",
                .partial_download_removals = {""},
                .delta_link = "https://graph.example.test/delta-invalid",
            });
            return fail("empty partial removal ID was accepted");
        } catch (const std::invalid_argument&) {
        }
        if (!database.partial_download("cleanup-drive", "keep-partial") ||
            database.delta_link("cleanup-drive") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-cleanup"
                }) {
            return fail("invalid partial removal was not rolled back");
        }

        const auto cleared = database.clear("me");
        if (cleared.items != 1 || cleared.pending_downloads != 1 ||
            cleared.partial_downloads != 1 || cleared.pending_moves != 1 ||
            cleared.upload_suppressions != 1 || cleared.blocked_items != 1 ||
            !cleared.delta_link || database.size() != 3 ||
            database.find("me", "fresh-me") ||
            !database.find("other-drive", "keep-me") ||
            !database.find("", "remote-1") || !database.find("", "remote-2") ||
            !database.pending_downloads("me").empty() ||
            database.pending_downloads("other-drive").size() != 1 ||
            !database.pending_moves("me").empty() ||
            database.pending_moves("other-drive").size() != 1 ||
            !database.upload_suppressions("me").empty() ||
            database.upload_suppressions("other-drive").size() != 1 ||
            database.partial_download("me", "partial-me") ||
            !database.partial_download("other-drive", "partial-other") ||
            !database.blocked_items("me").empty() ||
            database.blocked_items("other-drive").size() != 1 ||
            database.delta_link("me").has_value() ||
            database.delta_link("other-drive") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-other"
                }) {
            return fail("full clear did not isolate the configured drive");
        }
        database.remove_upload_suppression(
            "other-drive", temporary_directory.path() / "suppressed-other.txt"
        );
        if (!database.upload_suppressions("other-drive").empty()) {
            return fail("upload suppression removal was not persisted");
        }
    }
    return EXIT_SUCCESS;
}
