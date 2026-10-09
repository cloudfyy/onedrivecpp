#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace onedrive::gui {

struct ConfigurationPreview {
    std::filesystem::path sync_directory;
    std::filesystem::path state_directory;
    std::string sync_mode;
    std::string delete_policy;
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
