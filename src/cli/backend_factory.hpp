#pragma once

#include "onedrive/cli/backend.hpp"

#include <iosfwd>
#include <memory>

namespace onedrive::cli::detail {

[[nodiscard]] std::unique_ptr<ConsoleBackend> make_text_console_backend(
    ConsoleOptions options,
    std::ostream& output,
    std::ostream& error
);
[[nodiscard]] std::unique_ptr<ConsoleBackend> make_json_console_backend(
    ConsoleOptions options,
    std::ostream& output,
    std::ostream& error
);
[[nodiscard]] std::unique_ptr<ConsoleBackend> make_ftxui_console_backend(
    ConsoleOptions options,
    std::ostream& output,
    std::ostream& error,
    std::size_t columns,
    std::size_t rows
);

}  // namespace onedrive::cli::detail
