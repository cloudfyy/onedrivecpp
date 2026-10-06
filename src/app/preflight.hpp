#pragma once

#include "onedrive/util/unique_file_descriptor.hpp"
#include "onedrive/config/config.hpp"
#include "operation.hpp"

namespace onedrive::app::detail {

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
