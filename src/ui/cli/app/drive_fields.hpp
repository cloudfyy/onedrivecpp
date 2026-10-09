#pragma once

#include "onedrive/ui/cli/event.hpp"
#include "onedrive/graph/graph_client.hpp"

#include <optional>
#include <string>
#include <vector>

namespace onedrive::app::detail {

std::string format_bytes(std::uint64_t bytes);

std::vector<cli::Field>
quota_fields(const std::optional<graph::DriveQuota>& quota);

std::vector<cli::Field> drive_fields(
    const graph::DriveInfo& drive,
    std::optional<bool> configured = std::nullopt
);

} // namespace onedrive::app::detail
