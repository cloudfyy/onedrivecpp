#pragma once

#include "sync/upload/orchestration.hpp"
#include "sync/upload/planning.hpp"

namespace onedrive::sync::detail {

bool upload_directory(
    const UploadCandidate& upload,
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata,
    const cli::Console& console,
    UploadSummary& summary
);
void recover_pending_directory(
    const SafeSyncRoot& sync_root,
    storage::PendingUpload upload,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata
);

}  // namespace onedrive::sync::detail
