#pragma once

#include "onedrive/config/config.hpp"
#include "operation.hpp"

#include <filesystem>
#include <string_view>

namespace onedrive::app::detail {

void secure_state_directory(const std::filesystem::path& directory);
void validate_private_file(
    const std::filesystem::path& path,
    std::string_view description,
    bool required
);
void prepare_sync_directory(
    const config::Config& config,
    Operation operation
);
void validate_authentication_config(const config::Config& config);

}  // namespace onedrive::app::detail
