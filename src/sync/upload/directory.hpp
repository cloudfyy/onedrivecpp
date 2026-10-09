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
    const events::Observer& observer,
    UploadSummary& summary,
    const std::stop_token& stop_token = {}
);
void recover_pending_directory(
    const SafeSyncRoot& sync_root,
    storage::PendingUpload upload,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata,
    const std::stop_token& stop_token = {}
);

}  // namespace onedrive::sync::detail
