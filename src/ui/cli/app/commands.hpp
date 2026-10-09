#pragma once

#include "args.hpp"
#include "onedrive/app/factory.hpp"
#include "onedrive/ui/cli/console.hpp"
#include "onedrive/config/config.hpp"

namespace onedrive::app::detail {

[[nodiscard]] int execute_command(
    const Arguments& arguments,
    config::Config config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
);

}  // namespace onedrive::app::detail
