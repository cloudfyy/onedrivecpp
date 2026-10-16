#include "sync/core/reporting.hpp"

#include "onedrive/ui/common/observer.hpp"
#include "onedrive/storage/item_store.hpp"

namespace onedrive::sync::engine_detail {

void report_blocked(
    const storage::BlockedItem& item, const events::Observer& observer
) {
    observer.report_blocked_item(
        item.remote_path, item.reason_code, item.reason_message
    );
}

}  // namespace onedrive::sync::engine_detail
