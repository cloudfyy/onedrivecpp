#pragma once

#include "onedrive/config/config.hpp"

namespace onedrive::app::detail {

enum class Operation {
    authenticate,
    logout,
    reset_state,
    monitor,
    synchronize,
};

class RuntimePreflight {
public:
    RuntimePreflight(const config::Config& config, Operation operation);
    ~RuntimePreflight();

    RuntimePreflight(const RuntimePreflight&) = delete;
    RuntimePreflight& operator=(const RuntimePreflight&) = delete;
    RuntimePreflight(RuntimePreflight&&) = delete;
    RuntimePreflight& operator=(RuntimePreflight&&) = delete;

private:
    int lock_descriptor_{-1};
};

}  // namespace onedrive::app::detail
