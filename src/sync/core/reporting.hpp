#pragma once

namespace onedrive::events {
class Observer;
}

namespace onedrive::storage {
struct BlockedItem;
}

namespace onedrive::sync::engine_detail {

void report_blocked(
    const storage::BlockedItem& item, const events::Observer& observer
);

}  // namespace onedrive::sync::engine_detail
