#pragma once

#include "onedrive/app/factory.hpp"
#include "onedrive/ui/cli/console.hpp"
#include "onedrive/config/config.hpp"

#include <string>

namespace onedrive::app::detail {

[[nodiscard]] int show_shared(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
);

[[nodiscard]] int show_sites(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console,
    const std::string& query
);

} // namespace onedrive::app::detail
