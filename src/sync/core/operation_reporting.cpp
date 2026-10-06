#include "sync/core/operation_reporting.hpp"

#include "onedrive/cli/console.hpp"
#include "onedrive/storage/item_store.hpp"

#include <spdlog/spdlog.h>

namespace onedrive::sync::engine_detail {

void report_blocked(
    const storage::BlockedItem& item,
    const cli::Console& console
) {
    spdlog::warn(
        "Blocked remote item '{}': {} ({})",
        item.remote_path,
        item.reason_message,
        item.reason_code
    );
    console.blocked_item(
        item.remote_path,
        item.reason_code,
        item.reason_message
    );
}

}  // namespace onedrive::sync::engine_detail
