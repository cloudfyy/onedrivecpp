#include "download_recovery.hpp"

#include "local_filesystem.hpp"
#include "onedrive/remote_time.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>

namespace onedrive::sync::detail {
namespace {

bool recovery_file_matches(
    const std::filesystem::path& path,
    const storage::PendingDownload& download
) {
    std::error_code error;
    const bool regular = std::filesystem::is_regular_file(path, error);
    if (error) {
        throw std::runtime_error(
            "cannot inspect recovery file '" + path.string() + "': " +
            error.message()
        );
    }
    if (!regular) {
        return false;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw std::runtime_error(
            "cannot read recovery file size for '" + path.string() + "': " +
            error.message()
        );
    }
    return size == static_cast<std::uintmax_t>(download.item.size) &&
           content_fingerprint(path) == download.content_fingerprint;
}

void validate_pending_download(
    const storage::PendingDownload& download,
    const std::filesystem::path& sync_root
) {
    const auto destination = download.item.local_path.lexically_normal();
    const auto temporary = download.temporary_path.lexically_normal();
    const auto destination_relative = destination.lexically_relative(sync_root);
    const auto temporary_relative = temporary.lexically_relative(sync_root);
    const auto outside_root = [](const std::filesystem::path& relative) {
        return relative.empty() || relative.is_absolute() ||
               *relative.begin() == "..";
    };
    if (outside_root(destination_relative)) {
        throw std::runtime_error(
            "pending download destination escapes the synchronization root: " +
            destination.string()
        );
    }
    if (outside_root(temporary_relative) ||
        !paths_share_parent(temporary, destination)) {
        throw std::runtime_error(
            "pending download temporary path is outside the destination "
            "directory: " + temporary.string()
        );
    }
    const bool valid_fingerprint =
        download.content_fingerprint.size() == 64 &&
        std::ranges::all_of(
            download.content_fingerprint,
            [](char value) {
                return (value >= '0' && value <= '9') ||
                       (value >= 'a' && value <= 'f');
            }
        );
    if (download.item.directory || download.item.size < 0 ||
        download.item.last_modified.empty() || !valid_fingerprint) {
        throw std::runtime_error(
            "pending download journal contains invalid metadata for '" +
            download.item.remote_path + "'"
        );
    }
    static_cast<void>(
        parse_remote_modified_time(download.item.last_modified)
    );
}

}  // namespace

void recover_pending_downloads(
    storage::ItemStore& items,
    const std::filesystem::path& sync_root,
    const std::string& drive_id,
    const FilesystemMetadata& metadata
) {
    const auto pending = items.pending_downloads(drive_id);
    if (pending.empty()) {
        return;
    }
    spdlog::info(
        "Recovering {} pending downloads for drive '{}'",
        pending.size(),
        drive_id
    );
    for (auto download : pending) {
        validate_pending_download(download, sync_root);
        const auto destination = download.item.local_path;
        spdlog::debug(
            "Recovering pending download '{}' to '{}'",
            download.item.remote_path,
            destination.string()
        );
        ensure_directory_tree(sync_root, destination.parent_path());
        if (std::filesystem::is_symlink(
                std::filesystem::symlink_status(destination)
            ) ||
            std::filesystem::is_symlink(
                std::filesystem::symlink_status(download.temporary_path)
            )) {
            throw std::runtime_error(
                "pending download path contains a symbolic link"
            );
        }

        const bool temporary_exists =
            std::filesystem::exists(download.temporary_path);
        const bool destination_exists = std::filesystem::exists(destination);
        if (destination_exists &&
            recovery_file_matches(destination, download)) {
            if (temporary_exists) {
                if (!recovery_file_matches(download.temporary_path, download)) {
                    throw std::runtime_error(
                        "cannot remove recovery temporary file because it does "
                        "not match the journal: " +
                        download.temporary_path.string()
                    );
                }
                std::filesystem::remove(download.temporary_path);
                spdlog::debug(
                    "Removed obsolete recovery temporary file '{}'",
                    download.temporary_path.string()
                );
            }
            spdlog::debug(
                "Accepted already installed recovery destination '{}'",
                destination.string()
            );
        } else if (!destination_exists && temporary_exists &&
                   recovery_file_matches(download.temporary_path, download)) {
            std::filesystem::rename(download.temporary_path, destination);
            fsync_directory(destination.parent_path());
            spdlog::debug(
                "Promoted recovery temporary file to '{}'",
                destination.string()
            );
        } else {
            throw std::runtime_error(
                "cannot safely recover pending download '" +
                download.item.remote_path +
                "': neither destination nor temporary file matches the journal"
            );
        }

        graph::RemoteItem item{};
        item.id = download.item.remote_id;
        item.name = download.item.name;
        item.etag = download.item.etag;
        item.last_modified = download.item.last_modified;
        apply_remote_modified_time(destination, item.last_modified);
        metadata.write_remote_identity(item, destination);
        fsync_file(destination);
        download.item.local_size = static_cast<std::int64_t>(
            std::filesystem::file_size(destination)
        );
        download.item.local_modified_ticks = modified_ticks(destination);
        items.upsert(download.item);
        items.remove_pending_download(drive_id, download.item.remote_id);
        items.remove_partial_download(drive_id, download.item.remote_id);
        spdlog::info(
            "Recovered pending download '{}'",
            download.item.remote_path
        );
    }
}

}  // namespace onedrive::sync::detail
