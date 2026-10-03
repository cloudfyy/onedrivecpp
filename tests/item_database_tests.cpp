#include "onedrive/storage/item_database.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_{
              std::filesystem::temp_directory_path() /
              ("onedrive-cpp-item-database-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
          } {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    TemporaryDirectory temporary_directory;

    {
        onedrive::storage::ItemDatabase database{temporary_directory.path()};
        database.open();
        const auto database_permissions =
            std::filesystem::status(
                temporary_directory.path() / "items.sqlite3"
            ).permissions();
        if (database.size() != 0 ||
            (database_permissions & std::filesystem::perms::all) !=
                (std::filesystem::perms::owner_read |
                 std::filesystem::perms::owner_write)) {
            return fail("new state database is not empty");
        }

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
        database.upsert({
            .remote_id = "remote-1",
            .etag = "etag-updated",
            .local_path = "documents/report-renamed.txt",
        });
        database.apply_delta({
            .drive_id = "me",
            .upserts = {
                {
                    .remote_id = "remote-3",
                    .parent_id = "root-id",
                    .name = "notes.txt",
                    .etag = "etag-3",
                    .remote_path = "notes.txt",
                    .local_path = temporary_directory.path() / "notes.txt",
                    .last_modified = "2026-10-02T00:00:00Z",
                    .size = 42,
                    .local_size = 42,
                    .local_modified_ticks = 123456,
                    .directory = false,
                },
            },
            .delta_link = "https://graph.example.test/delta-1",
        });

        if (database.size() != 3 ||
            database.delta_link("me") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-1"
                }) {
            return fail("upsert did not preserve the expected item count");
        }
    }

    {
        onedrive::storage::ItemDatabase database{temporary_directory.path()};
        database.open();

        const auto* first = database.find("", "remote-1");
        const auto* second = database.find("", "remote-2");
        const auto* third = database.find("me", "remote-3");
        if (database.size() != 3 || first == nullptr || second == nullptr ||
            third == nullptr) {
            return fail("persisted items were not loaded");
        }
        if (first->etag != "etag-updated" ||
            first->local_path != "documents/report-renamed.txt") {
            return fail("updated item state was not persisted");
        }
        if (second->etag != "etag-2" || second->local_path != "photos/image.jpg") {
            return fail("second item state was not persisted");
        }
        if (third->drive_id != "me" || third->parent_id != "root-id" ||
            third->remote_path != "notes.txt" || third->size != 42 ||
            third->local_size != 42 ||
            third->local_modified_ticks != 123456 || third->directory ||
            database.delta_link("me") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-1"
                }) {
            return fail("delta item state was not persisted");
        }

        try {
            database.apply_delta({
                .drive_id = "me",
                .upserts = {
                    {
                        .remote_id = "rolled-back",
                        .name = "rolled-back.txt",
                        .etag = "rolled-back-etag",
                        .remote_path = "rolled-back.txt",
                        .local_path =
                            temporary_directory.path() / "rolled-back.txt",
                    },
                },
                .removals = {""},
                .delta_link = "https://graph.example.test/delta-invalid",
            });
            return fail("invalid delta state was accepted");
        } catch (const std::invalid_argument&) {
        }
        if (database.find("me", "rolled-back") != nullptr ||
            database.delta_link("me") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-1"
                }) {
            return fail("failed delta update was not rolled back");
        }

        database.apply_delta({
            .drive_id = "me",
            .removals = {"remote-3"},
            .delta_link = "https://graph.example.test/delta-2",
        });
        if (database.size() != 2 || database.find("me", "remote-3") != nullptr ||
            database.delta_link("me") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-2"
                }) {
            return fail("delta removal was not persisted");
        }
    }

    {
        onedrive::storage::ItemDatabase database{temporary_directory.path()};
        database.open();
        if (database.size() != 2 || database.find("me", "remote-3") != nullptr ||
            database.delta_link("me") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-2"
                }) {
            return fail("updated delta state was not loaded");
        }

        database.apply_delta({
            .drive_id = "me",
            .upserts = {
                {
                    .remote_id = "reset-me",
                    .name = "reset-me.txt",
                    .etag = "reset-me-etag",
                    .remote_path = "reset-me.txt",
                    .local_path = temporary_directory.path() / "reset-me.txt",
                },
            },
            .delta_link = "https://graph.example.test/delta-me",
        });
        database.apply_delta({
            .drive_id = "other-drive",
            .upserts = {
                {
                    .remote_id = "keep-me",
                    .name = "keep-me.txt",
                    .etag = "keep-me-etag",
                    .remote_path = "keep-me.txt",
                    .local_path = temporary_directory.path() / "keep-me.txt",
                },
            },
            .delta_link = "https://graph.example.test/delta-other",
        });
        database.save_pending_download({
            .item = {
                .drive_id = "me",
                .remote_id = "pending-me",
                .name = "pending-me.txt",
                .etag = "pending-etag",
                .remote_path = "pending-me.txt",
                .local_path = temporary_directory.path() / "pending-me.txt",
                .size = 4,
            },
            .temporary_path = temporary_directory.path() / "pending-me.tmp",
            .content_fingerprint = "fingerprint-me",
        });
        database.save_pending_download({
            .item = {
                .drive_id = "other-drive",
                .remote_id = "pending-other",
                .name = "pending-other.txt",
                .etag = "pending-etag",
                .remote_path = "pending-other.txt",
                .local_path = temporary_directory.path() / "pending-other.txt",
                .size = 5,
            },
            .temporary_path = temporary_directory.path() / "pending-other.tmp",
            .content_fingerprint = "fingerprint-other",
        });
        if (database.pending_downloads("me").size() != 1 ||
            database.pending_downloads("other-drive").size() != 1) {
            return fail("pending downloads were not saved by drive");
        }

        if (!database.reset("me") || database.size() != 4 ||
            database.find("me", "reset-me") == nullptr ||
            database.find("other-drive", "keep-me") == nullptr ||
            database.delta_link("me").has_value() ||
            database.pending_downloads("me").size() != 1 ||
            database.pending_downloads("other-drive").size() != 1 ||
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
        onedrive::storage::ItemDatabase database{temporary_directory.path()};
        database.open();
        if (database.size() != 4 ||
            database.find("me", "reset-me") == nullptr ||
            database.find("other-drive", "keep-me") == nullptr ||
            database.delta_link("me").has_value() ||
            database.pending_downloads("me").size() != 1 ||
            database.pending_downloads("other-drive").size() != 1 ||
            database.delta_link("other-drive") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-other"
                }) {
            return fail("safe cursor reset was not persisted");
        }

        database.apply_delta({
            .drive_id = "me",
            .upserts = {
                {
                    .remote_id = "fresh-me",
                    .name = "fresh-me.txt",
                    .etag = "fresh-me-etag",
                    .remote_path = "fresh-me.txt",
                    .local_path = temporary_directory.path() / "fresh-me.txt",
                },
            },
            .delta_link = "https://graph.example.test/delta-fresh",
            .replace_drive_items = true,
        });
        if (database.size() != 4 ||
            database.find("me", "reset-me") != nullptr ||
            database.find("me", "fresh-me") == nullptr ||
            database.find("other-drive", "keep-me") == nullptr ||
            database.find("", "remote-1") == nullptr ||
            database.find("", "remote-2") == nullptr ||
            database.pending_downloads("me").size() != 1 ||
            database.pending_downloads("other-drive").size() != 1) {
            return fail("initial delta did not replace only the selected drive");
        }

        const auto cleared = database.clear("me");
        if (cleared.items != 1 || cleared.pending_downloads != 1 ||
            !cleared.delta_link || database.size() != 3 ||
            database.find("me", "fresh-me") != nullptr ||
            database.find("other-drive", "keep-me") == nullptr ||
            database.find("", "remote-1") == nullptr ||
            database.find("", "remote-2") == nullptr ||
            !database.pending_downloads("me").empty() ||
            database.pending_downloads("other-drive").size() != 1 ||
            database.delta_link("me").has_value() ||
            database.delta_link("other-drive") !=
                std::optional<std::string>{
                    "https://graph.example.test/delta-other"
                }) {
            return fail("full clear did not isolate the configured drive");
        }
    }

    if (!std::filesystem::exists(temporary_directory.path() / "items.sqlite3")) {
        return fail("SQLite state database was not created");
    }

    return EXIT_SUCCESS;
}
