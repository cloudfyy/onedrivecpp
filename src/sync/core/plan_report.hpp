#pragma once

#include "onedrive/sync/capabilities.hpp"
#include "sync/core/plan.hpp"

#include <string>

namespace onedrive::cli {
class Console;
}

namespace onedrive::sync::engine_detail {

void report_plan(
    const detail::SyncPlan& plan,
    const std::string& drive_id,
    const cli::Console& console,
    SyncCapabilities capabilities
);

}  // namespace onedrive::sync::engine_detail
