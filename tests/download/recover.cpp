#include "support.hpp"

namespace {

using namespace onedrive::test::download;

int test_recover() {
    namespace detail = onedrive::sync::detail;
    DownloadFixture fixture;
    auto& temporary = fixture.temporary;
    const auto& root = fixture.root;
    const auto& metadata = fixture.metadata;
    const auto& safe_root = fixture.safe_root;
    auto& space = fixture.space;
    auto& graph = fixture.graph;
    auto& items = fixture.items;

    const auto duplicate_destination = root / "duplicate.txt";
    const auto duplicate_temporary = root / ".duplicate.partial";
    for (const auto& path : {duplicate_destination, duplicate_temporary}) {
        std::ofstream output{path, std::ios::binary};
        output << "data";
    }
    items.pending.emplace(
        "duplicate",
        onedrive::storage::PendingDownload{
            .item =
                {
                    .drive_id = "me",
                    .remote_id = "duplicate",
                    .name = "duplicate.txt",
                    .etag = "etag",
                    .remote_path = "duplicate.txt",
                    .local_path = duplicate_destination,
                    .last_modified = "2026-10-02T00:00:00Z",
                    .size = 4,
                },
            .temporary_path = duplicate_temporary,
            .content_fingerprint = "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602"
                                   "261a9a923d3bb23adc8b7",
        }
    );
    detail::recover_pending_downloads(items, root, "me", metadata);
    if (std::filesystem::exists(duplicate_temporary) ||
        items.pending.contains("duplicate")) {
        return fail("duplicate recovery temporary file was not cleaned");
    }

    const auto replacing_destination = root / "replace-old.txt";
    const auto replacing_temporary = root / ".replace-old.partial";
    {
        std::ofstream destination_output{
            replacing_destination, std::ios::binary
        };
        destination_output << "old!";
        std::ofstream temporary_output{replacing_temporary, std::ios::binary};
        temporary_output << "data";
    }
    const auto old_baseline =
        detail::capture_local_file_baseline(replacing_destination);
    items.states.emplace(
        "replace-old",
        onedrive::storage::ItemState{
            .drive_id = "me",
            .remote_id = "replace-old",
            .name = "replace-old.txt",
            .etag = "old-etag",
            .remote_path = "replace-old.txt",
            .local_path = replacing_destination,
            .last_modified = "2026-10-01T00:00:00Z",
            .size = 4,
            .local_size = old_baseline.size,
            .local_modified_ticks = old_baseline.modified_ticks,
        }
    );
    items.pending.emplace(
        "replace-old",
        onedrive::storage::PendingDownload{
            .item =
                {
                    .drive_id = "me",
                    .remote_id = "replace-old",
                    .name = "replace-old.txt",
                    .etag = "new-etag",
                    .remote_path = "replace-old.txt",
                    .local_path = replacing_destination,
                    .last_modified = "2026-10-02T00:00:00Z",
                    .size = 4,
                },
            .temporary_path = replacing_temporary,
            .content_fingerprint =
                detail::content_fingerprint(replacing_temporary),
        }
    );
    detail::recover_pending_downloads(items, root, "me", metadata);
    std::ifstream replaced_input{replacing_destination, std::ios::binary};
    const std::string replaced_contents{
        std::istreambuf_iterator<char>{replaced_input},
        std::istreambuf_iterator<char>{}
    };
    if (replaced_contents != "data" || items.pending.contains("replace-old")) {
        return fail(
            "journal recovery did not replace a trusted previous snapshot"
        );
    }

    const auto backed_up_destination = root / "backed-up.txt";
    const auto backed_up_temporary = root / ".backed-up.partial";
    const auto backup_path =
        root / "backed-up.safeBackup-20261004T051000Z-0001.txt";
    {
        std::ofstream destination_output{
            backed_up_destination, std::ios::binary
        };
        destination_output << "user";
        std::ofstream backup_output{backup_path, std::ios::binary};
        backup_output << "user";
        std::ofstream temporary_output{backed_up_temporary, std::ios::binary};
        temporary_output << "data";
    }
    items.pending.emplace(
        "backed-up",
        onedrive::storage::PendingDownload{
            .item =
                {
                    .drive_id = "me",
                    .remote_id = "backed-up",
                    .name = "backed-up.txt",
                    .etag = "etag",
                    .remote_path = "backed-up.txt",
                    .local_path = backed_up_destination,
                    .last_modified = "2026-10-02T00:00:00Z",
                    .size = 4,
                },
            .temporary_path = backed_up_temporary,
            .content_fingerprint =
                detail::content_fingerprint(backed_up_temporary),
            .backup_path = backup_path,
            .backup_fingerprint = detail::content_fingerprint(backup_path),
        }
    );
    detail::recover_pending_downloads(items, root, "me", metadata);
    std::ifstream recovered_input{backed_up_destination, std::ios::binary};
    const std::string recovered_contents{
        std::istreambuf_iterator<char>{recovered_input},
        std::istreambuf_iterator<char>{}
    };
    if (recovered_contents != "data" ||
        detail::content_fingerprint(backup_path) ==
            detail::content_fingerprint(backed_up_destination) ||
        items.pending.contains("backed-up")) {
        return fail("safeBackup recovery did not promote remote content");
    }

    const auto damaged_destination = root / "damaged-backup.txt";
    const auto damaged_temporary = root / ".damaged-backup.partial";
    const auto damaged_backup =
        root / "damaged-backup.safeBackup-20261004T051000Z-0001.txt";
    for (const auto& [path, contents] :
         std::vector<std::pair<std::filesystem::path, std::string>>{
             {damaged_destination, "user"},
             {damaged_backup, "user"},
             {damaged_temporary, "data"},
         }) {
        std::ofstream output{path, std::ios::binary};
        output << contents;
    }
    const auto expected_backup_fingerprint =
        detail::content_fingerprint(damaged_backup);
    {
        std::ofstream output{damaged_backup, std::ios::binary};
        output << "tampered";
    }
    items.pending.emplace(
        "damaged-backup",
        onedrive::storage::PendingDownload{
            .item =
                {
                    .drive_id = "me",
                    .remote_id = "damaged-backup",
                    .name = "damaged-backup.txt",
                    .etag = "etag",
                    .remote_path = "damaged-backup.txt",
                    .local_path = damaged_destination,
                    .last_modified = "2026-10-02T00:00:00Z",
                    .size = 4,
                },
            .temporary_path = damaged_temporary,
            .content_fingerprint =
                detail::content_fingerprint(damaged_temporary),
            .backup_path = damaged_backup,
            .backup_fingerprint = expected_backup_fingerprint,
        }
    );
    try {
        detail::recover_pending_downloads(items, root, "me", metadata);
        return fail("damaged safeBackup recovery was accepted");
    } catch (const std::runtime_error&) {
    }
    if (detail::content_fingerprint(damaged_destination) !=
        expected_backup_fingerprint) {
        return fail("damaged safeBackup recovery overwrote local content");
    }
    items.pending.erase("damaged-backup");

    items.pending.emplace(
        "invalid",
        onedrive::storage::PendingDownload{
            .item =
                {
                    .drive_id = "me",
                    .remote_id = "invalid",
                    .name = "invalid.txt",
                    .etag = "etag",
                    .remote_path = "invalid.txt",
                    .local_path = root / "invalid.txt",
                    .last_modified = "2026-10-02T00:00:00Z",
                    .size = 4,
                },
            .temporary_path = root / ".invalid.partial",
            .content_fingerprint = "not-a-sha256",
        }
    );
    try {
        detail::recover_pending_downloads(items, root, "me", metadata);
        return fail("invalid recovery journal metadata was accepted");
    } catch (const std::runtime_error&) {
    }
    items.pending.erase("invalid");

    items.pending.emplace(
        "outside",
        onedrive::storage::PendingDownload{
            .item =
                {
                    .drive_id = "me",
                    .remote_id = "outside",
                    .name = "outside.txt",
                    .etag = "etag",
                    .remote_path = "outside.txt",
                    .local_path = temporary.path() / "outside.txt",
                    .last_modified = "2026-10-02T00:00:00Z",
                    .size = 4,
                },
            .temporary_path = temporary.path() / ".outside.partial",
            .content_fingerprint = "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602"
                                   "261a9a923d3bb23adc8b7",
        }
    );
    try {
        detail::recover_pending_downloads(items, root, "me", metadata);
        return fail("recovery destination outside the sync root was accepted");
    } catch (const std::runtime_error&) {
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_recover();
}
