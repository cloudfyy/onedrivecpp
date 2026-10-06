#pragma once

#include "onedrive/graph/graph_client.hpp"
#include "sync/upload/planning.hpp"

#include <cstdint>
#include <stop_token>
#include <string>

namespace onedrive::sync::detail {

class FilesystemMetadata;

enum class FileUploadStatus {
    skipped,
    uploaded,
    blocked,
};

struct FileUploadResult {
    FileUploadStatus status;
    std::string message;
};

[[nodiscard]] FileUploadResult execute_file_upload(
    const UploadCandidate& upload,
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata,
    std::uint64_t previous_failure_count,
    const std::stop_token& stop_token
);

}  // namespace onedrive::sync::detail
