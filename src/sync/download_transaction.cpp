#include "download_transaction.hpp"

#include "download_integrity.hpp"
#include "local_filesystem.hpp"

#include <spdlog/spdlog.h>

#include <format>
#include <stdexcept>

namespace onedrive::sync::detail {

PreparedDownload prepare_download(
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const graph::RemoteItem& item,
    storage::ItemState state,
    const std::filesystem::path& destination,
    LocalFileBaseline destination_baseline,
    const FilesystemMetadata& metadata,
    DownloadSpaceCoordinator& space,
    const graph::DownloadProgress& progress
) {
    if (item.size < 0) {
        throw std::invalid_argument(
            "cannot download an item with a negative size"
        );
    }
    auto temporary = temporary_path_for(destination);
    std::uint64_t completed_bytes = 0;
    const auto saved =
        items.partial_download(state.drive_id, item.id);
    if (saved.has_value()) {
        const auto& partial = saved.value();
        const auto normalized_temporary =
            partial.temporary_path.lexically_normal();
        const auto normalized_destination = destination.lexically_normal();
        if (!is_temporary_path_for(
                normalized_destination,
                normalized_temporary
            )) {
            throw std::runtime_error(
                "partial download path is outside the destination directory: " +
                partial.temporary_path.string()
            );
        }

        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(
            partial.temporary_path,
            status_error
        );
        const bool missing =
            status_error == std::errc::no_such_file_or_directory ||
            (!status_error && !std::filesystem::exists(status));
        if (status_error && !missing) {
            throw std::runtime_error(
                "cannot inspect partial download '" +
                partial.temporary_path.string() + "': " +
                status_error.message()
            );
        }
        const bool metadata_matches =
            partial.item.etag == item.etag &&
            partial.item.size == item.size &&
            partial.item.local_path.lexically_normal() ==
                normalized_destination &&
            partial.completed_bytes <=
                static_cast<std::uint64_t>(item.size);
        if (!missing && std::filesystem::is_regular_file(status) &&
            metadata_matches) {
            const auto actual_size =
                std::filesystem::file_size(partial.temporary_path);
            if (actual_size >= partial.completed_bytes) {
                if (actual_size > partial.completed_bytes) {
                    std::filesystem::resize_file(
                        partial.temporary_path,
                        partial.completed_bytes
                    );
                }
                temporary = partial.temporary_path;
                completed_bytes = partial.completed_bytes;
                spdlog::info(
                    "Resuming partial download '{}' at byte {}",
                    item.remote_path,
                    completed_bytes
                );
            }
        }
        if (completed_bytes == 0) {
            std::error_code remove_error;
            std::filesystem::remove(partial.temporary_path, remove_error);
            if (remove_error) {
                throw std::runtime_error(
                    "cannot discard stale partial download '" +
                    partial.temporary_path.string() + "': " +
                    remove_error.message()
                );
            }
            items.remove_partial_download(state.drive_id, item.id);
        }
    }

    auto space_reservation = space.acquire(
        static_cast<std::uintmax_t>(item.size) - completed_bytes
    );
    state.local_path = destination;
    items.save_partial_download({
        .item = state,
        .temporary_path = temporary,
        .completed_bytes = completed_bytes,
    });
    spdlog::trace(
        "Downloading '{}' through a same-directory temporary file at byte {}",
        item.remote_path,
        completed_bytes
    );
    try {
        graph.download_file(
            item.id,
            static_cast<std::uint64_t>(item.size),
            temporary,
            completed_bytes,
            progress,
            [&](std::uint64_t durable_bytes) {
                if (durable_bytes < completed_bytes) {
                    throw std::logic_error(
                        "download checkpoint moved backwards"
                    );
                }
                const auto newly_durable =
                    durable_bytes - completed_bytes;
                items.save_partial_download({
                    .item = state,
                    .temporary_path = temporary,
                    .completed_bytes = durable_bytes,
                });
                space_reservation.consume(newly_durable);
                completed_bytes = durable_bytes;
            }
        );
        const auto downloaded_size = std::filesystem::file_size(temporary);
        if (downloaded_size != static_cast<std::uintmax_t>(item.size)) {
            throw std::runtime_error(
                std::format(
                    "downloaded size mismatch for '{}': expected {}, received {}",
                    item.remote_path,
                    item.size,
                    downloaded_size
                )
            );
        }
        const std::string fingerprint = content_fingerprint(temporary);
        verify_download_integrity(temporary, item, fingerprint);
        metadata.write_remote_identity(item, temporary);
        return {
            .item = item,
            .state = std::move(state),
            .destination = destination,
            .temporary_path = temporary,
            .content_fingerprint = fingerprint,
            .downloaded_size = downloaded_size,
            .destination_baseline = std::move(destination_baseline),
            .space_reservation = std::move(space_reservation),
        };
    } catch (const DownloadIntegrityError& error) {
        items.remove_partial_download(state.drive_id, item.id);
        PreparedDownload incomplete;
        incomplete.temporary_path = temporary;
        discard_prepared_download(incomplete);
        spdlog::warn(
            "Download integrity verification failed for '{}': {}",
            item.remote_path,
            error.what()
        );
        throw;
    } catch (const std::exception& error) {
        if (completed_bytes == 0) {
            items.remove_partial_download(state.drive_id, item.id);
            PreparedDownload incomplete;
            incomplete.temporary_path = temporary;
            discard_prepared_download(incomplete);
        }
        spdlog::warn(
            "Download preparation failed for '{}' at byte {}: {}",
            item.remote_path,
            completed_bytes,
            error.what()
        );
        throw;
    } catch (...) {
        if (completed_bytes == 0) {
            items.remove_partial_download(state.drive_id, item.id);
            PreparedDownload incomplete;
            incomplete.temporary_path = temporary;
            discard_prepared_download(incomplete);
        }
        spdlog::warn(
            "Download preparation failed for '{}' at byte {} due to an "
            "unknown error",
            item.remote_path,
            completed_bytes
        );
        throw;
    }
}

storage::ItemState commit_download(
    storage::ItemStore& items,
    PreparedDownload download
) {
    bool journaled = false;
    try {
        const auto ensure_destination_unchanged = [&] {
            if (!local_file_matches_baseline(
                    download.destination,
                    download.destination_baseline
                )) {
                throw LocalModificationConflictError(
                    "local file changed while downloading: " +
                    download.destination.string()
                );
            }
        };
        ensure_destination_unchanged();
        download.state.local_path = download.destination;
        items.save_pending_download({
            .item = download.state,
            .temporary_path = download.temporary_path,
            .content_fingerprint = download.content_fingerprint,
        });
        journaled = true;
        items.remove_partial_download(
            download.state.drive_id,
            download.state.remote_id
        );
        if (!local_file_matches_baseline(
                download.destination,
                download.destination_baseline
            )) {
            items.remove_pending_download(
                download.state.drive_id,
                download.state.remote_id
            );
            items.remove_partial_download(
                download.state.drive_id,
                download.state.remote_id
            );
            journaled = false;
            throw LocalModificationConflictError(
                "local file changed while committing the download: " +
                download.destination.string()
            );
        }
        std::filesystem::rename(
            download.temporary_path,
            download.destination
        );
        fsync_directory(download.destination.parent_path());
        download.state.local_size =
            static_cast<std::int64_t>(download.downloaded_size);
        download.state.local_modified_ticks =
            modified_ticks(download.destination);
        items.upsert(download.state);
        items.remove_pending_download(
            download.state.drive_id,
            download.state.remote_id
        );
        spdlog::debug(
            "Atomically installed '{}' ({} bytes)",
            download.item.remote_path,
            download.downloaded_size
        );
        return download.state;
    } catch (const std::exception& error) {
        if (!journaled) {
            discard_prepared_download(download);
        }
        spdlog::warn(
            "Download installation failed for '{}'; {}: {}",
            download.item.remote_path,
            journaled ? "recovery journal retained" :
                        "no recovery journal was created",
            error.what()
        );
        throw;
    } catch (...) {
        if (!journaled) {
            discard_prepared_download(download);
        }
        spdlog::warn(
            "Download installation failed for '{}'; {} due to an unknown error",
            download.item.remote_path,
            journaled ? "recovery journal retained" :
                        "no recovery journal was created"
        );
        throw;
    }
}

void discard_prepared_download(const PreparedDownload& download) noexcept {
    if (download.temporary_path.empty()) {
        return;
    }
    std::error_code cleanup_error;
    std::filesystem::remove(download.temporary_path, cleanup_error);
    if (cleanup_error) {
        spdlog::warn(
            "Could not remove incomplete download '{}': {}",
            download.temporary_path.string(),
            cleanup_error.message()
        );
    }
}

storage::ItemState download_atomically(
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const graph::RemoteItem& item,
    storage::ItemState state,
    const std::filesystem::path& destination,
    const FilesystemMetadata& metadata,
    DownloadSpaceCoordinator& space,
    const graph::DownloadProgress& progress
) {
    return commit_download(
        items,
        prepare_download(
            graph,
            items,
            item,
            std::move(state),
            destination,
            capture_local_file_baseline(destination),
            metadata,
            space,
            progress
        )
    );
}

}  // namespace onedrive::sync::detail
