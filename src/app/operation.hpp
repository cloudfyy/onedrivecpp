#pragma once

namespace onedrive::app::detail {

enum class Operation {
    authenticate,
    logout,
    diagnose,
    drives,
    quota,
    status,
    reset_state,
    download,
    monitor,
    synchronize,
};

}  // namespace onedrive::app::detail
