#include "download_transaction.hpp"

#include "local_filesystem.hpp"

#include <spdlog/spdlog.h>

#include <format>
#include <stdexcept>

namespace onedrive::sync::detail {

PreparedDownload prepare_download(
    graph::GraphClient& graph,
    const graph::RemoteItem& item,
    storage::ItemState state,
    const std::filesystem::path& destination,
    const FilesystemMetadata& metadata,
    const graph::DownloadProgress& progress
) {
    const auto temporary = temporary_path_for(destination);
    spdlog::trace(
        "Downloading '{}' through a same-directory temporary file",
        item.remote_path
    );
    try {
        graph.download_file(
            item.id,
            static_cast<std::uint64_t>(item.size),
            temporary,
            progress
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
        metadata.write_remote_identity(item, temporary);
        return {
            .item = item,
            .state = std::move(state),
            .destination = destination,
            .temporary_path = temporary,
            .content_fingerprint = fingerprint,
            .downloaded_size = downloaded_size,
        };
    } catch (const std::exception& error) {
        PreparedDownload incomplete;
        incomplete.temporary_path = temporary;
        discard_prepared_download(incomplete);
        spdlog::warn(
            "Download preparation failed for '{}': {}",
            item.remote_path,
            error.what()
        );
        throw;
    } catch (...) {
        PreparedDownload incomplete;
        incomplete.temporary_path = temporary;
        discard_prepared_download(incomplete);
        spdlog::warn(
            "Download preparation failed for '{}' due to an unknown error",
            item.remote_path
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
        download.state.local_path = download.destination;
        items.save_pending_download({
            .item = download.state,
            .temporary_path = download.temporary_path,
            .content_fingerprint = download.content_fingerprint,
        });
        journaled = true;
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
    const graph::DownloadProgress& progress
) {
    return commit_download(
        items,
        prepare_download(
            graph,
            item,
            std::move(state),
            destination,
            metadata,
            progress
        )
    );
}

}  // namespace onedrive::sync::detail
