#include "cli/terminal.hpp"
#include "support/common.hpp"

#include <cstdlib>
#include <string>

namespace {

using onedrive::cli::detail::TerminalCapabilities;
using onedrive::cli::detail::probe_terminal;
using onedrive::cli::detail::supports_tui;
using onedrive::cli::detail::tui_unavailable_reason;
using onedrive::test::fail;

int test_terminal_capabilities() {
    const auto detected = probe_terminal();
    if (supports_tui(detected) !=
        tui_unavailable_reason(detected).empty()) {
        return fail("terminal probe produced inconsistent capabilities");
    }

    const TerminalCapabilities supported{
        .input_is_terminal = true,
        .output_is_terminal = true,
        .term_is_supported = true,
        .columns = 60,
        .rows = 12,
    };
    if (!supports_tui(supported) ||
        !tui_unavailable_reason(supported).empty()) {
        return fail("supported terminal was rejected");
    }

    auto capabilities = supported;
    capabilities.input_is_terminal = false;
    if (supports_tui(capabilities) ||
        !tui_unavailable_reason(capabilities).contains(
            "standard input"
        )) {
        return fail("non-terminal input was accepted");
    }

    capabilities = supported;
    capabilities.output_is_terminal = false;
    if (supports_tui(capabilities) ||
        !tui_unavailable_reason(capabilities).contains(
            "standard output"
        )) {
        return fail("non-terminal output was accepted");
    }

    capabilities = supported;
    capabilities.term_is_supported = false;
    if (supports_tui(capabilities) ||
        !tui_unavailable_reason(capabilities).contains("TERM")) {
        return fail("unsupported TERM was accepted");
    }

    capabilities = supported;
    capabilities.columns = 59;
    if (supports_tui(capabilities) ||
        !tui_unavailable_reason(capabilities).contains("60 columns")) {
        return fail("narrow terminal was accepted");
    }

    capabilities = supported;
    capabilities.rows = 11;
    if (supports_tui(capabilities)) {
        return fail("short terminal was accepted");
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    return test_terminal_capabilities();
}
