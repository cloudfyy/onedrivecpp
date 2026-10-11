#pragma once

#include "onedrive/sync/capabilities.hpp"

#include <filesystem>
#include <optional>

namespace onedrive::gui {

struct ConfigurationPreview {
    std::filesystem::path sync_directory;
    std::filesystem::path state_directory;
    sync::SyncMode sync_mode;
    sync::DeletePolicy delete_policy;
};

[[nodiscard]] ConfigurationPreview preview_configuration(
    const std::filesystem::path& source_file,
    const std::filesystem::path& destination_file
);

[[nodiscard]] std::optional<std::filesystem::path> save_basic_settings(
    const std::filesystem::path& config_file,
    const std::filesystem::path& sync_directory,
    const std::filesystem::path& state_directory
);

[[nodiscard]] std::optional<std::filesystem::path> import_configuration(
    const std::filesystem::path& source_file,
    const std::filesystem::path& destination_file
);

} // namespace onedrive::gui
