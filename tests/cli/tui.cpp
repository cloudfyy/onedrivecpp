#include "cli/backend_factory.hpp"
#include "onedrive/cli/console.hpp"
#include "support/common.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {

using onedrive::test::fail;

int test_tui_dashboard() {
    using namespace onedrive::cli;
    using onedrive::util::ProgressState;

    std::ostringstream output;
    std::ostringstream error;
    auto backend = detail::make_ftxui_console_backend(
        {.ui = UiMode::tui}, output, error, 100
    );
    if (backend->output_mode() != OutputMode::text ||
        backend->ui_mode() != UiMode::tui) {
        return fail("FTXUI backend reported the wrong mode");
    }

    const Console console{std::move(backend)};
    console.message(MessageKind::information, "phase", "Planning changes");
    console.delta_progress(2, 350, ProgressState::ongoing);
    console.delta_summary({
        .pages = 2,
        .scanned_items = 350,
        .unique_changes = 340,
        .files = 300,
        .directories = 30,
        .deletions = 10,
    });
    console.download_progress(
        1,
        4,
        512,
        1'024,
        ProgressState::ongoing,
        {.bytes_per_second = 128}
    );
    console.section(
        "summary",
        "Sync summary",
        {{.label = "Files:", .key = "files", .value = "4"}}
    );
    console.blocked_item(
        "conflict.txt", "local_modification", "changed locally"
    );
    console.message(MessageKind::success, "complete", "Sync complete");

    const auto rendered = output.str();
    if (!rendered.contains("onedrive-cpp sync") ||
        !rendered.contains("Microsoft Graph") ||
        !rendered.contains("350 items") ||
        !rendered.contains("Downloads: 1/4 files") ||
        !rendered.contains("50%") ||
        !rendered.contains("512 B / 1.0 KiB") ||
        !rendered.contains("Sync summary") ||
        !rendered.contains("Files: 4") ||
        !rendered.contains("Blocked: 1") ||
        !rendered.contains("conflict.txt") ||
        !rendered.contains("Sync complete") ||
        !rendered.contains("\033[?25l") ||
        !error.str().empty()) {
        return fail("FTXUI dashboard did not render event state");
    }
    std::istringstream confirmation{"yes\n"};
    auto* original_input = std::cin.rdbuf(confirmation.rdbuf());
    const bool confirmed =
        console.confirm("confirm", "Continue? ", "yes");
    std::cin.rdbuf(original_input);
    if (!confirmed) {
        return fail("FTXUI confirmation did not accept expected input");
    }
    console.end_download_progress();

    std::ostringstream unstyled_output;
    auto unstyled_backend = detail::make_ftxui_console_backend(
        {
            .color = ColorMode::never,
            .ui = UiMode::tui,
        },
        unstyled_output,
        error,
        80
    );
    unstyled_backend->emit(MessageEvent{
        .kind = MessageKind::success,
        .event = "complete",
        .text = "Complete",
    });
    unstyled_backend.reset();
    const auto unstyled = unstyled_output.str();
    if (unstyled.contains("\033[1m") ||
        unstyled.contains("\033[3") ||
        unstyled.contains("\033[38;") ||
        !unstyled.contains("\033[?25h")) {
        return fail("color-disabled TUI retained ANSI styling");
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    return test_tui_dashboard();
}
