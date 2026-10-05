#include "sync/download/target.hpp"

#include "sync/filesystem/local.hpp"

#include <utility>

namespace onedrive::sync::detail {

storage::ItemState item_state_for(
    const std::string& drive_id,
    const graph::RemoteItem& item,
    std::filesystem::path local_path
) {
    return {
        .drive_id = drive_id,
        .remote_id = item.id,
        .parent_id = item.parent_id,
        .name = item.name,
        .etag = item.etag,
        .ctag = item.ctag,
        .remote_path = item.remote_path,
        .local_path = std::move(local_path),
        .last_modified = item.last_modified,
        .size = item.size,
        .local_size = 0,
        .local_modified_ticks = 0,
        .directory = item.directory,
    };
}

DownloadTargetStatus inspect_download_target(
    const std::optional<storage::ItemState>& previous,
    const graph::RemoteItem& item,
    const std::filesystem::path& destination
) {
    const bool exists = std::filesystem::exists(destination);
    const bool snapshot_matches =
        exists && previous.has_value() &&
        local_snapshot_matches(*previous, destination);
    return {
        .current_remote_file =
            snapshot_matches &&
            remote_content_version_matches(
                *previous,
                item.etag,
                item.ctag
            ),
        .preserve_local = exists && !snapshot_matches,
    };
}

}  // namespace onedrive::sync::detail
