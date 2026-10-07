#pragma once

#include "onedrive/cli/event.hpp"
#include "onedrive/graph/graph_client.hpp"

#include <optional>
#include <vector>

namespace onedrive::app::detail {

std::vector<cli::Field> drive_fields(
    const graph::DriveInfo& drive,
    std::optional<bool> configured = std::nullopt
);

} // namespace onedrive::app::detail
