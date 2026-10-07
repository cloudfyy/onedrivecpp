#include "cli/terminal.hpp"

#include <cstdlib>
#include <string_view>
#include <sys/ioctl.h>
#include <unistd.h>

namespace onedrive::cli::detail {
namespace {

constexpr std::size_t minimum_columns = 60;
constexpr std::size_t minimum_rows = 12;

}  // namespace

TerminalCapabilities probe_terminal() noexcept {
    TerminalCapabilities capabilities{
        .input_is_terminal = ::isatty(STDIN_FILENO) != 0,
        .output_is_terminal = ::isatty(STDOUT_FILENO) != 0,
    };
    const char* term = std::getenv("TERM");
    capabilities.term_is_supported =
        term != nullptr && std::string_view{term} != "dumb";

    winsize size{};
    if (capabilities.output_is_terminal &&
        ::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
        capabilities.columns = size.ws_col;
        capabilities.rows = size.ws_row;
    }
    return capabilities;
}

bool supports_tui(
    const TerminalCapabilities& capabilities
) noexcept {
    return capabilities.input_is_terminal &&
           capabilities.output_is_terminal &&
           capabilities.term_is_supported &&
           capabilities.columns >= minimum_columns &&
           capabilities.rows >= minimum_rows;
}

std::string tui_unavailable_reason(
    const TerminalCapabilities& capabilities
) {
    if (!capabilities.input_is_terminal) {
        return "standard input is not connected to a terminal";
    }
    if (!capabilities.output_is_terminal) {
        return "standard output is not connected to a terminal";
    }
    if (!capabilities.term_is_supported) {
        return "TERM is unset or does not support terminal controls";
    }
    if (capabilities.columns < minimum_columns ||
        capabilities.rows < minimum_rows) {
        return "terminal size must be at least 60 columns by 12 rows";
    }
    return {};
}

}  // namespace onedrive::cli::detail
