#include "download_transaction.hpp"

#include "local_filesystem.hpp"

#include <spdlog/spdlog.h>

#include <format>
#include <stdexcept>

namespace onedrive::sync::detail {

storage::ItemState download_atomically(
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const graph::RemoteItem& item,
    storage::ItemState state,
    const std::filesystem::path& destination,
    const FilesystemMetadata& metadata
) {
    const auto temporary = temporary_path_for(destination);
    bool journaled = false;
    spdlog::trace(
        "Downloading '{}' through a same-directory temporary file",
        item.remote_path
    );
    try {
        graph.download_file(item.id, temporary);
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
        state.local_path = destination;
        items.save_pending_download({
            .item = state,
            .temporary_path = temporary,
            .content_fingerprint = fingerprint,
        });
        journaled = true;
        std::filesystem::rename(temporary, destination);
        fsync_directory(destination.parent_path());
        state.local_size = static_cast<std::int64_t>(downloaded_size);
        state.local_modified_ticks = modified_ticks(destination);
        items.upsert(state);
        items.remove_pending_download(state.drive_id, state.remote_id);
        spdlog::debug(
            "Atomically installed '{}' ({} bytes)",
            item.remote_path,
            downloaded_size
        );
        return state;
    } catch (const std::exception& error) {
        if (!journaled) {
            std::error_code cleanup_error;
            std::filesystem::remove(temporary, cleanup_error);
            if (cleanup_error) {
                spdlog::warn(
                    "Could not remove incomplete download '{}': {}",
                    temporary.string(),
                    cleanup_error.message()
                );
            }
        }
        spdlog::warn(
            "Download installation failed for '{}'; {}: {}",
            item.remote_path,
            journaled ? "recovery journal retained" :
                        "no recovery journal was created",
            error.what()
        );
        throw;
    } catch (...) {
        if (!journaled) {
            std::error_code cleanup_error;
            std::filesystem::remove(temporary, cleanup_error);
            if (cleanup_error) {
                spdlog::warn(
                    "Could not remove incomplete download '{}': {}",
                    temporary.string(),
                    cleanup_error.message()
                );
            }
        }
        spdlog::warn(
            "Download installation failed for '{}'; {} due to an unknown error",
            item.remote_path,
            journaled ? "recovery journal retained" :
                        "no recovery journal was created"
        );
        throw;
    }
}

}  // namespace onedrive::sync::detail
