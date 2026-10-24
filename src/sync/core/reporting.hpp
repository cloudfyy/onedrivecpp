#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/storage/item_store.hpp"

#include <chrono>
#include <cstddef>

namespace onedrive::events {
class Observer;
}

namespace onedrive::sync::detail {
struct UploadSummary;
}

namespace onedrive::sync::engine_detail {

struct ExecutionSummary;

void report_blocked(
    const storage::BlockedItem& item, const events::Observer& observer
);

void report_dry_run_configuration(
    const config::Config& config,
    storage::ItemStore& items,
    const events::Observer& observer
);
void report_upload_plan(
    const config::Config& config,
    const detail::UploadSummary& summary,
    const events::Observer& observer
);
void report_execution_summary(
    const ExecutionSummary& execution,
    const detail::UploadSummary& upload,
    std::size_t blocked,
    std::size_t unsupported,
    const events::Observer& observer
);
void report_completion(
    std::size_t blocked,
    bool preview,
    std::chrono::steady_clock::time_point started_at,
    const events::Observer& observer
);

}  // namespace onedrive::sync::engine_detail
