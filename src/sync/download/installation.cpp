#include "sync/download/installation.hpp"

#include "sync/filesystem/operations.hpp"
#include "sync/filesystem/safe_backup.hpp"

#include <spdlog/spdlog.h>

#include <optional>
#include <string_view>

namespace onedrive::sync::detail {
namespace {

bool matches_content(
    const std::filesystem::path& path,
    std::string_view fingerprint,
    std::optional<std::int64_t> expected_size = std::nullopt
) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw std::filesystem::filesystem_error(
            "cannot inspect download installation file", path, error
        );
    }
    if (!std::filesystem::is_regular_file(status)) {
        return false;
    }
    const auto baseline = capture_local_file_baseline(path);
    return baseline.existed &&
           (!expected_size || baseline.size == *expected_size) &&
           baseline.fingerprint == fingerprint;
}

} // namespace

void install_pending_download(
    storage::ItemStore& items,
    const SafeSyncRoot& sync_root,
    storage::PendingDownload& download,
    config::LocalConflictPolicy local_conflict,
    std::move_only_function<void(const std::filesystem::path&)> backup_created
) {
    const auto& destination = download.item.local_path;
    const auto& temporary = download.temporary_path;
    if (destination.lexically_normal() == temporary.lexically_normal()) {
        throw std::invalid_argument(
            "download destination and temporary path must be distinct"
        );
    }
    const auto conflict = [&] {
        return LocalModificationConflictError(
            "local file changed while installing the download: " +
            destination.string()
        );
    };
    const auto record_backup = [&](const SafeBackup& backup) {
        download.backup_path = backup.path;
        download.backup_fingerprint = backup.fingerprint;
        items.save_pending_download(download);
        spdlog::warn(
            "Preserved concurrent local edit as '{}'", backup.path.string()
        );
        if (backup_created) {
            backup_created(backup.path);
        }
    };
    const auto finish_exchange = [&] {
        try {
            const auto displaced = capture_local_file_baseline(temporary);
            if (!displaced.existed) {
                throw conflict();
            }
            if (displaced.fingerprint != download.replacement_fingerprint) {
                if (local_conflict == config::LocalConflictPolicy::block) {
                    throw conflict();
                }
                record_backup(
                    preserve_displaced_backup(sync_root, temporary, destination)
                );
            } else {
                if (!download.backup_path.empty() &&
                    !matches_content(
                        download.backup_path, download.backup_fingerprint
                    )) {
                    throw std::runtime_error(
                        "cannot discard displaced local file: safeBackup "
                        "does not match the download journal: " +
                        download.backup_path.string()
                    );
                }
                remove_no_symlinks(temporary, MissingPathPolicy::report);
            }
            sync_root.fsync_directory(destination.parent_path());
        } catch (...) {
            // Exchange back, never overwrite: a second edit remains in the
            // journaled temporary instead of being discarded by rollback.
            if (std::filesystem::exists(
                    std::filesystem::symlink_status(temporary)
                )) {
                try {
                    sync_root.exchange(temporary, destination);
                } catch (const std::filesystem::filesystem_error& error) {
                    if (error.code() != std::errc::no_such_file_or_directory) {
                        throw;
                    }
                    if (!sync_root.rename_no_replace(temporary, destination)) {
                        sync_root.exchange(temporary, destination);
                    }
                }
                sync_root.fsync_directory(destination.parent_path());
                spdlog::warn(
                    "Restored local download destination '{}'; recovery "
                    "journal and any remaining temporary '{}' retained",
                    destination.string(),
                    temporary.string()
                );
            }
            throw;
        }
    };

    if (matches_content(
            destination, download.content_fingerprint, download.item.size
        )) {
        const auto status = std::filesystem::symlink_status(temporary);
        if (std::filesystem::exists(status)) {
            if (!download.replacement_fingerprint.empty()) {
                finish_exchange();
            } else if (matches_content(
                           temporary,
                           download.content_fingerprint,
                           download.item.size
                       )) {
                remove_no_symlinks(temporary, MissingPathPolicy::report);
                sync_root.fsync_directory(destination.parent_path());
            } else {
                throw std::runtime_error(
                    "recovery temporary does not match the download journal: " +
                    temporary.string()
                );
            }
        }
        return;
    }
    if (!matches_content(
            temporary, download.content_fingerprint, download.item.size
        )) {
        throw std::runtime_error(
            "cannot install download: temporary file does not match the "
            "journal: " +
            temporary.string()
        );
    }

    if (download.replacement_fingerprint.empty()) {
        if (sync_root.rename_no_replace(temporary, destination)) {
            sync_root.fsync_directory(destination.parent_path());
            return;
        }
        if (local_conflict == config::LocalConflictPolicy::block) {
            throw conflict();
        }
    }

    const auto baseline = capture_local_file_baseline(destination);
    if (!baseline.existed) {
        throw conflict();
    }
    if (baseline.fingerprint != download.replacement_fingerprint) {
        if (local_conflict == config::LocalConflictPolicy::block) {
            throw conflict();
        }
        const auto backup =
            preserve_safe_backup(sync_root, destination, baseline);
        download.replacement_fingerprint = baseline.fingerprint;
        record_backup(backup);
    }
    try {
        sync_root.exchange(temporary, destination);
    } catch (const std::filesystem::filesystem_error& error) {
        if (error.code() == std::errc::no_such_file_or_directory) {
            throw conflict();
        }
        throw;
    }
    sync_root.fsync_directory(destination.parent_path());
    finish_exchange();
}

} // namespace onedrive::sync::detail
