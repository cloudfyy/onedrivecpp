#pragma once

#include "onedrive/app/factory.hpp"
#include "onedrive/cli/console.hpp"
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

} // namespace onedrive::app::detail
