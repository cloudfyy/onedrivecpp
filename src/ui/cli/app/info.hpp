#pragma once

#include "onedrive/app/factory.hpp"
#include "onedrive/ui/cli/console.hpp"
#include "onedrive/config/config.hpp"

namespace onedrive::app::detail {

[[nodiscard]] int show_drives(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
);

[[nodiscard]] int show_quota(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
);

[[nodiscard]] int show_status(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
);

[[nodiscard]] int show_storage(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
);

[[nodiscard]] int show_partials(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
);

[[nodiscard]] int show_files(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console,
    std::string_view path,
    const std::optional<std::string>& status
);

[[nodiscard]] int verify_files(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console,
    std::string_view path,
    std::string_view mode
);

[[nodiscard]] int show_config(
    const config::Config& config,
    const std::filesystem::path& config_path,
    const cli::Console& console
);

[[nodiscard]] int cleanup_state(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console,
    bool dry_run,
    bool assume_yes
);

} // namespace onedrive::app::detail
