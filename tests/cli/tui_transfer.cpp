#include "ui/cli/backend_factory.hpp"
#include "ui/cli/app/drive_fields.hpp"
#include "ui/cli/format.hpp"
#include "onedrive/util/byte_size.hpp"
#include "onedrive/ui/cli/console.hpp"
#include "support/common.hpp"
#include "cli/partials_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <limits>
#include <sstream>
#include <string>
#include "tui_cases.hpp"

namespace onedrive::test::tui_cases {

using onedrive::test::fail;

int test_adaptive_sync_layout() {
    using namespace onedrive::cli;
    using onedrive::events::SyncPhase;
    for (const auto rows : {12U, 18U, 24U, 40U}) {
        std::ostringstream output;
        std::ostringstream error;
        const Console console{detail::make_ftxui_console_backend(
            {.color = ColorMode::never,
             .ui = UiMode::tui,
             .view = TuiView::watch},
            output,
            error,
            120,
            rows
        )};
        const auto frame = [&] {
            const auto rendered = output.str();
            return rendered.substr(rendered.rfind("\033[H"));
        };
        console.operation_state(
            OperationKind::monitoring, OperationState::watching
        );
        console.sync_context(onedrive::sync::SyncMode::bidirectional, false);
        console.sync_phase(SyncPhase::uploading);
        console.section("execution_summary", "Empty summary", {});
        console.message(
            MessageKind::information, "activity", "Recent activity"
        );
        console.blocked_item("blocked.one", "unsupported", "Unsupported file");
        console.upload_progress(
            {.completed_files = 0,
             .file_count = 1,
             .uploaded = 1024,
             .total = 1024}
        );
        const auto active = frame();
        if (!active.contains("UPLOADS  0/1 files") ||
            !active.contains("MONITORING: uploading") ||
            !active.contains("NEEDS ATTENTION") ||
            !active.contains("q / Esc") ||
            !active.contains("Recent activity") ||
            active.contains("Empty summary") ||
            (rows == 12 && !active.contains("// 99%"))) {
            return fail(
                "adaptive TUI lost priority content or retained an empty panel "
                "at " +
                std::to_string(rows) + " rows:\n" + active
            );
        }
        console.file_operation({
            .action = onedrive::events::FileAction::uploaded,
            .path = "finished.txt",
        });
        console.sync_summary({.uploaded = 1, .blocked = 1});
        console.sync_phase(SyncPhase::waiting);
        const auto waiting = frame();
        if (waiting.contains("UPLOADS") || waiting.contains("DOWNLOADS") ||
            waiting.contains("Empty summary") ||
            !waiting.contains("MONITORING: waiting") ||
            !waiting.contains("NEEDS ATTENTION") ||
            !waiting.contains("q / Esc") ||
            (rows >= 18 && !waiting.contains("finished.txt"))) {
            return fail(
                "waiting TUI retained transfer panels or lost available history"
            );
        }
        // Only the outer frame remains when there is no active transfer.
        const auto corner = waiting.find("╭");
        if (corner == std::string::npos ||
            waiting.find("╭", corner + std::string{"╭"}.size()) !=
                std::string::npos) {
            return fail(
                "waiting TUI rendered an empty or unnecessary inner frame"
            );
        }
        console.sync_phase(SyncPhase::uploading);
        console.section(
            "details",
            "Oversized details",
            {
                {.label = "First:", .key = "first", .value = "one"},
                {.label = "Second:", .key = "second", .value = "two"},
                {.label = "Third:", .key = "third", .value = "three"},
            }
        );
        const auto details = frame();
        if (rows == 12 && details.contains("Oversized details")) {
            return fail("short TUI did not hide details that cannot fit");
        }
        if (rows == 40 && (!details.contains("Oversized details") ||
                           !details.contains("Third: three"))) {
            return fail("tall TUI hid details despite sufficient space");
        }
        console.delta_progress(1, 10, onedrive::util::ProgressState::ongoing);
        console.download_progress(
            0, 1, 512, 1024, onedrive::util::ProgressState::ongoing
        );
        const auto simultaneous = frame();
        if (!simultaneous.contains("UPLOADS") ||
            !simultaneous.contains("DOWNLOADS") ||
            !simultaneous.contains("MONITORING: uploading") ||
            !simultaneous.contains("NEEDS ATTENTION") ||
            !simultaneous.contains("q / Esc")) {
            return fail(
                "simultaneous transfers displaced essential TUI controls"
            );
        }
        console.upload_progress(
            {.completed_files = 1,
             .file_count = 1,
             .uploaded = 1024,
             .total = 1024,
             .state = onedrive::util::ProgressState::completed}
        );
        if (frame().contains("UPLOADS") || !frame().contains("DOWNLOADS")) {
            return fail(
                "completed monitor transfer displaced an active transfer"
            );
        }
    }
    return EXIT_SUCCESS;
}

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
            MessageKind::information, "log", "Microsoft Graph request details"
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
            1, 4, 512, 1'024, ProgressState::ongoing, {.bytes_per_second = 128}
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
            MessageKind::information, "sync_completed", "Sync completed"
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
            !rendered.contains("cloud service cloud check needs attention") ||
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
        const bool confirmed = console.confirm("confirm", "Continue? ", "yes");
        std::cin.rdbuf(original_input);
        if (!confirmed) {
            return fail("FTXUI confirmation did not accept expected input");
        }
        try {
            console.message(
                static_cast<MessageKind>(255), "invalid", "Invalid"
            );
            return fail("FTXUI accepted an unknown message kind");
        } catch (const std::logic_error&) {
        }
    }
    if (!output.str().contains("\033[?1049l")) {
        return fail("FTXUI did not restore the original terminal screen");
    }

    std::ostringstream download_output;
    auto download_backend = detail::make_ftxui_console_backend(
        {
            .color = ColorMode::never,
            .ui = UiMode::tui,
            .view = TuiView::download,
        },
        download_output,
        error,
        80,
        20
    );
    download_backend.reset();
    if (!download_output.str().contains("ONEDRIVE // DOWNLOAD  v")) {
        return fail("download TUI did not render its command title");
    }

    std::ostringstream auth_output;
    auto auth_backend = detail::make_ftxui_console_backend(
        {
            .color = ColorMode::never,
            .ui = UiMode::tui,
            .view = TuiView::auth,
        },
        auth_output,
        error,
        80,
        20
    );
    auth_backend->emit(
        MessageEvent{
            .kind = MessageKind::information,
            .event = "device_authorization",
            .text =
                "To sign in, use a web browser to open the page "
                "https://login.microsoftonline.com/device and enter the code "
                "ABCD-EFGH to authenticate.",
        }
    );
    auth_backend->emit(
        MessageEvent{
            .kind = MessageKind::information,
            .event = "authorization_wait",
            .text = "Waiting for authorization...",
        }
    );
    auth_backend.reset();
    if (!auth_output.str().contains("ONEDRIVE // AUTH  v") ||
        !auth_output.str().contains("ABCD-EFGH") ||
        !auth_output.str().contains("Waiting for authorization...")) {
        return fail("auth TUI did not wrap the device code and status");
    }

    std::ostringstream doctor_output;
    auto doctor_backend = detail::make_ftxui_console_backend(
        {
            .color = ColorMode::never,
            .ui = UiMode::tui,
            .view = TuiView::health,
        },
        doctor_output,
        error,
        80,
        20
    );
    doctor_backend->emit(
        SectionEvent{
            .event = "database_integrity",
            .title = "Synchronization state database:",
            .fields = {
                {.label = "status:", .key = "status", .value = "healthy"},
            },
        }
    );
    doctor_backend.reset();
    if (!doctor_output.str().contains("ONEDRIVE // HEALTH  v") ||
        !doctor_output.str().contains("status: healthy")) {
        return fail("health TUI did not render diagnostic status");
    }

    std::ostringstream status_output;
    auto status_backend = detail::make_ftxui_console_backend(
        {
            .color = ColorMode::never,
            .ui = UiMode::tui,
            .view = TuiView::status,
        },
        status_output,
        error,
        80,
        24
    );
    status_backend->emit(
        SectionEvent{
            .event = "status",
            .title = "Synchronization status:",
            .fields = {
                {.label = "account:", .key = "account", .value = "Test User"},
                {.label = "tracked:", .key = "tracked_items", .value = "42"},
            },
        }
    );
    status_backend.reset();
    if (!status_output.str().contains("ONEDRIVE // STATUS  v") ||
        !status_output.str().contains("account: Test User") ||
        !status_output.str().contains("tracked: 42")) {
        return fail("status TUI did not render synchronization status");
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
    unstyled_backend->emit(
        MessageEvent{
            .kind = MessageKind::information,
            .event = "complete",
            .text = "Complete",
        }
    );
    unstyled_backend.reset();
    const auto unstyled = unstyled_output.str();
    if (unstyled.contains("\033[1m") || unstyled.contains("\033[3") ||
        unstyled.contains("\033[38;") || !unstyled.contains("\033[?25h")) {
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
            .view = TuiView::watch,
        },
        monitor_output,
        error,
        80,
        20
    );
    monitor_backend->emit(
        MessageEvent{
            .kind = MessageKind::information,
            .event = "monitor_ready",
            .text = "Monitoring local and Microsoft Graph changes for: /sync",
        }
    );
    monitor_backend.reset();
    if (!monitor_output.str().contains("ONEDRIVE // WATCH  v") ||
        !monitor_output.str().contains("q / Esc  EXIT") ||
        !monitor_output.str().contains(
            "Watching for local and cloud changes in: /sync"
        ) ||
        monitor_output.str().contains("Microsoft Graph")) {
        return fail("monitor TUI did not render keyboard exit guidance");
    }
    return EXIT_SUCCESS;
}

int test_message_levels() {
    using namespace onedrive::cli;
    for (const auto kind :
         {MessageKind::trace,
          MessageKind::debug,
          MessageKind::information,
          MessageKind::warning,
          MessageKind::error,
          MessageKind::critical}) {
        std::ostringstream output;
        std::ostringstream error;
        const Console console{detail::make_ftxui_console_backend(
            {.color = ColorMode::always, .ui = UiMode::tui},
            output,
            error,
            100,
            30
        )};
        console.message(kind, "log", "backend detail");
        const auto logged = output.str();
        const auto frame = logged.substr(logged.rfind("\033[H"));
        const bool suppressed = kind == MessageKind::trace ||
                                kind == MessageKind::debug ||
                                kind == MessageKind::information;
        if (frame.contains("backend detail") == suppressed) {
            return fail("TUI changed backend log severity filtering");
        }
        console.message(kind, "application", "application detail");
        const auto rendered = output.str();
        const auto application_frame =
            rendered.substr(rendered.rfind("\033[H"));
        if (!application_frame.contains("application detail") ||
            !error.str().empty()) {
            return fail("TUI lost an application message level");
        }
    }
    {
        std::ostringstream output;
        std::ostringstream error;
        const Console console{detail::make_ftxui_console_backend(
            {.ui = UiMode::tui}, output, error, 100, 30
        )};
        if (!onedrive::test::throws_with<std::logic_error>(
                [&] {
                    console.message(
                        static_cast<MessageKind>(255), "log", "invalid level"
                    );
                },
                "unknown console message kind"
            )) {
            return fail("TUI accepted an unknown backend log severity");
        }
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::tui_cases
