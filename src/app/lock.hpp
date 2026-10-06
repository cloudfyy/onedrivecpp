#pragma once

#include "onedrive/util/unique_file_descriptor.hpp"

#include <filesystem>

namespace onedrive::app::detail {

[[nodiscard]] onedrive::util::UniqueFD acquire_runtime_lock(
    const std::filesystem::path& state_directory
);

}  // namespace onedrive::app::detail
