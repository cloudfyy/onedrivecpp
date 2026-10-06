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

} // namespace onedrive::app::detail
