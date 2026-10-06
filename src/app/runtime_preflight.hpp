#pragma once

#include "util/unique_file_descriptor.hpp"
#include "onedrive/config/config.hpp"

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

class RuntimePreflight {
public:
    RuntimePreflight(const config::Config& config, Operation operation);
    ~RuntimePreflight() = default;

    RuntimePreflight(const RuntimePreflight&) = delete;
    RuntimePreflight& operator=(const RuntimePreflight&) = delete;
    RuntimePreflight(RuntimePreflight&&) = delete;
    RuntimePreflight& operator=(RuntimePreflight&&) = delete;

private:
    onedrive::util::UniqueFD lock_descriptor_;
};

}  // namespace onedrive::app::detail
