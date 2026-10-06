#pragma once

namespace onedrive::app::detail {

enum class Operation {
    authenticate,
    logout,
    diagnose,
    reset_state,
    download,
    monitor,
    synchronize,
};

}  // namespace onedrive::app::detail
