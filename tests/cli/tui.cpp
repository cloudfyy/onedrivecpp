#include "cli/backend_factory.hpp"
#include "onedrive/cli/console.hpp"
#include "support/common.hpp"

#include <array>
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
        {.ui = UiMode::tui}, output, error, 100, 30
    );
    if (backend->output_mode() != OutputMode::text ||
        backend->ui_mode() != UiMode::tui) {
        return fail("FTXUI backend reported the wrong mode");
    }

    if (!output.str().contains("\033[?1049h") ||
        !output.str().contains("ONEDRIVE // SYNC  v") ||
        !output.str().contains("HACKER")) {
        return fail("FTXUI did not enter the full-screen dashboard");
    }
    {
        const Console console{std::move(backend)};
        console.message(
            MessageKind::information,
            "delta_query_started",
            "Checking the cloud for changes..."
        );
        console.message(
            MessageKind::information,
            "log",
            "Microsoft Graph request details"
        );
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
        console.message(
            MessageKind::success, "sync_completed", "Sync completed"
        );
        console.message(
            MessageKind::warning,
            "cloud_warning",
            "Cloud storage needs attention"
        );

        const auto rendered = output.str();
        if (rendered.contains("Microsoft Graph") ||
            !rendered.contains("CLOUD CHECK  350 items checked") ||
            !rendered.contains("340 changes found") ||
            !rendered.contains("DOWNLOADS  1/4 files") ||
            !rendered.contains("50%") ||
            !rendered.contains("512 B / 1.0 KiB") ||
            !rendered.contains("Sync summary") ||
            !rendered.contains("Files: 4") ||
            !rendered.contains("NEEDS ATTENTION  1") ||
            !rendered.contains("conflict.txt") ||
            !rendered.contains("Sync completed") ||
            !rendered.contains("Checking the cloud for changes") ||
            !rendered.contains("Cloud storage needs attention") ||
            !error.str().empty()) {
            return fail("FTXUI dashboard did not render friendly state");
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
    }
    if (!output.str().contains("\033[?1049l")) {
        return fail("FTXUI did not restore the original terminal screen");
    }

    std::ostringstream unstyled_output;
    auto unstyled_backend = detail::make_ftxui_console_backend(
        {
            .color = ColorMode::never,
            .ui = UiMode::tui,
        },
        unstyled_output,
        error,
        80,
        20
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

    struct ThemeCase {
        TuiTheme theme;
        std::string_view name;
    };
    constexpr std::array themes{
        ThemeCase{TuiTheme::hacker, "HACKER"},
        ThemeCase{TuiTheme::ocean, "OCEAN"},
        ThemeCase{TuiTheme::amber, "AMBER"},
        ThemeCase{TuiTheme::synthwave, "SYNTHWAVE"},
    };
    for (const auto& theme : themes) {
        std::ostringstream themed_output;
        auto themed_backend = detail::make_ftxui_console_backend(
            {
                .color = ColorMode::always,
                .ui = UiMode::tui,
                .theme = theme.theme,
            },
            themed_output,
            error,
            80,
            20
        );
        themed_backend.reset();
        if (!themed_output.str().contains(theme.name)) {
            return fail("FTXUI theme was not rendered");
        }
    }

    std::ostringstream monitor_output;
    auto monitor_backend = detail::make_ftxui_console_backend(
        {
            .color = ColorMode::never,
            .ui = UiMode::tui,
            .view = TuiView::monitor,
        },
        monitor_output,
        error,
        80,
        20
    );
    monitor_backend->emit(MessageEvent{
        .kind = MessageKind::success,
        .event = "monitor_ready",
        .text = "Watching for local and cloud changes.",
    });
    monitor_backend.reset();
    if (!monitor_output.str().contains("ONEDRIVE // MONITOR  v") ||
        !monitor_output.str().contains("q / Esc  EXIT") ||
        !monitor_output.str().contains(
            "Watching for local and cloud changes."
        ) ||
        monitor_output.str().contains("Microsoft Graph")) {
        return fail("monitor TUI did not render keyboard exit guidance");
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    return test_tui_dashboard();
}
