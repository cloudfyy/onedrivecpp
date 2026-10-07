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
        output.str().contains("HACKER")) {
        return fail("FTXUI did not enter the full-screen dashboard");
    }
    {
        const Console console{std::move(backend)};
        console.message(
            MessageKind::information,
            "delta_query_started",
            "Starting Microsoft Graph delta query"
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
        console.end_download_progress();
        const auto after_end = output.str();
        const auto frame_after_end =
            after_end.substr(after_end.rfind("\033[H"));
        if (!frame_after_end.contains("DOWNLOADS  1/4 files") ||
            !frame_after_end.contains("ETA calculating...") ||
            !frame_after_end.contains("[")) {
            return fail("FTXUI removed completed download progress");
        }
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
            "Microsoft Graph Delta needs attention"
        );
        console.download_progress(
            2,
            4,
            768,
            1'024,
            ProgressState::ongoing,
            {
                .bytes_per_second = 256,
                .estimated_seconds_remaining = 65,
            }
        );
        console.end_download_progress();

        const auto rendered = output.str();
        const auto final_frame = rendered.substr(rendered.rfind("\033[H"));
        if (final_frame.contains("Microsoft Graph") ||
            !rendered.contains("CLOUD CHECK  350 items checked") ||
            !rendered.contains("340 changes found") ||
            !final_frame.contains("DOWNLOADS  2/4 files") ||
            !final_frame.contains("75%") ||
            !final_frame.contains("768 B / 1.0 KiB") ||
            !final_frame.contains("ETA 00:01:05") ||
            !rendered.contains("Sync summary") ||
            !rendered.contains("Files: 4") ||
            !rendered.contains("NEEDS ATTENTION  1") ||
            !rendered.contains("conflict.txt") ||
            !rendered.contains("Sync complete.") ||
            !rendered.contains("Checking the cloud for changes") ||
            !rendered.contains(
                "cloud service cloud check needs attention"
            ) ||
            !error.str().empty()) {
            return fail("FTXUI dashboard did not render friendly state");
        }
        console.download_progress(
            4,
            4,
            1'024,
            1'024,
            ProgressState::completed,
            {
                .bytes_per_second = 256,
                .elapsed_milliseconds = 1'500,
            }
        );
        const auto completed_output = output.str();
        const auto completed_frame =
            completed_output.substr(completed_output.rfind("\033[H"));
        if (!completed_frame.contains("DOWNLOADS  4/4 files") ||
            !completed_frame.contains("elapsed 00:00:01") ||
            completed_frame.contains("ETA ")) {
            return fail("FTXUI did not render completed download timing");
        }
        std::istringstream confirmation{"yes\n"};
        auto* original_input = std::cin.rdbuf(confirmation.rdbuf());
        const bool confirmed =
            console.confirm("confirm", "Continue? ", "yes");
        std::cin.rdbuf(original_input);
        if (!confirmed) {
            return fail("FTXUI confirmation did not accept expected input");
        }
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
        if (themed_output.str().contains(theme.name) ||
            !themed_output.str().contains("ONEDRIVE // SYNC")) {
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
        .text = "Monitoring local and Microsoft Graph changes for: /sync",
    });
    monitor_backend.reset();
    if (!monitor_output.str().contains("ONEDRIVE // MONITOR  v") ||
        !monitor_output.str().contains("q / Esc  EXIT") ||
        !monitor_output.str().contains(
            "Watching for local and cloud changes in: /sync"
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
