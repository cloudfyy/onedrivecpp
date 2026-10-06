#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace onedrive::storage {

struct StateSummary {
    bool database_present{false};
    std::size_t tracked_items{0};
    std::size_t blocked_items{0};
    std::size_t pending_downloads{0};
    std::size_t partial_downloads{0};
    std::size_t pending_uploads{0};
    std::size_t pending_deletes{0};
    std::size_t pending_remote_moves{0};
    std::size_t pending_local_moves{0};
    bool delta_cursor{false};
    std::string sync_filter_fingerprint;
};

[[nodiscard]] StateSummary read_state_summary(
    const std::filesystem::path& drive_state_directory,
    const std::string& drive_id
);

} // namespace onedrive::storage
