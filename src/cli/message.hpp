#pragma once

#include "onedrive/cli/event.hpp"

#include <string_view>

namespace onedrive::cli::detail {

[[nodiscard]] std::string_view message_level(MessageKind kind);
[[nodiscard]] bool message_suppressed(
    const ConsoleOptions& options,
    MessageKind kind
);
[[noreturn]] void throw_unknown_message_kind();

}  // namespace onedrive::cli::detail
