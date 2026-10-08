#pragma once

#include "onedrive/sync/capabilities.hpp"
#include "sync/core/plan.hpp"

#include <string>

namespace onedrive::events {
class Observer;
}

namespace onedrive::sync::engine_detail {

void report_plan(
    const detail::SyncPlan& plan,
    const std::string& drive_id,
    const events::Observer& observer,
    SyncCapabilities capabilities
);

}  // namespace onedrive::sync::engine_detail
