#pragma once

#include <cstddef>
#include <string>

namespace onedrive::cli::detail {

struct TerminalCapabilities {
    bool input_is_terminal{false};
    bool output_is_terminal{false};
    bool term_is_supported{false};
    std::size_t columns{0};
    std::size_t rows{0};
};

[[nodiscard]] TerminalCapabilities probe_terminal() noexcept;
[[nodiscard]] bool supports_tui(
    const TerminalCapabilities& capabilities
) noexcept;
[[nodiscard]] std::string tui_unavailable_reason(
    const TerminalCapabilities& capabilities
);

}  // namespace onedrive::cli::detail
