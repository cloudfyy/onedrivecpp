#pragma once

namespace onedrive::app::detail {

enum class Operation {
    authenticate,
    logout,
    diagnose,
    drives,
    shared,
    sites,
    quota,
    status,
    reset_cursor,
    clear_state,
    download,
    monitor,
    synchronize,
};

} // namespace onedrive::app::detail
