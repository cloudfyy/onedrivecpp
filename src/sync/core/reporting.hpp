#pragma once

namespace onedrive::cli {
class Console;
}

namespace onedrive::storage {
struct BlockedItem;
}

namespace onedrive::sync::engine_detail {

void report_blocked(
    const storage::BlockedItem& item,
    const cli::Console& console
);

}  // namespace onedrive::sync::engine_detail
