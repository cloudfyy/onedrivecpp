#include "cli/backend_factory.hpp"
#include "app/drive_fields.hpp"
#include "onedrive/cli/console.hpp"
#include "support/common.hpp"
#include "cli/partials_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {

using onedrive::test::fail;

int test_inspection_completion() {
    using namespace onedrive::cli;
    constexpr std::array inspection_views{
        TuiView::health, TuiView::status, TuiView::drives, TuiView::shared,
        TuiView::sites, TuiView::quota, TuiView::storage, TuiView::partials,
        TuiView::files, TuiView::verify, TuiView::config,
    };
    for (const auto view : inspection_views) {
        for (const auto* keys : {
                 "\nremaining\n",
                 "q\nremaining\n",
                 "Q\nremaining\n",
                 "x\nn\np\nq\nremaining\n",
             }) {
            std::ostringstream output;
            std::ostringstream error;
            {
                const Console console{detail::make_ftxui_console_backend(
                    {.color = ColorMode::never,
                     .ui = UiMode::tui,
                     .view = view},
                    output,
                    error,
                    100,
                    30
                )};
                console.message(
                    MessageKind::success, "result", "Inspection result retained"
                );
                std::istringstream input{keys};
                auto* original = std::cin.rdbuf(input.rdbuf());
                console.finish();
                console.finish();
                std::cin.rdbuf(original);
                const auto rendered = output.str();
                const auto frame = rendered.substr(rendered.rfind("\033[H"));
                if (!frame.contains("Press Enter to exit (or q)") ||
                    !frame.contains("Inspection result retained") ||
                    rendered.contains("\033[?1049l") || input.peek() != 'r') {
                    return fail(
                        "inspection completion lost results or read twice"
                    );
                }
            }
            if (!output.str().contains("\033[?1049l")) {
                return fail(
                    "inspection completion did not restore the terminal"
                );
            }
        }
    }
    for (const auto view :
         {TuiView::sync, TuiView::download, TuiView::watch, TuiView::auth}) {
        std::ostringstream output;
        std::ostringstream error;
        const Console console{detail::make_ftxui_console_backend(
            {.ui = UiMode::tui, .view = view}, output, error, 80, 20
        )};
        std::istringstream input{"untouched\n"};
        auto* original = std::cin.rdbuf(input.rdbuf());
        console.finish();
        std::cin.rdbuf(original);
        if (input.peek() != 'u' ||
            output.str().contains("Press Enter to exit")) {
            return fail("non-inspection TUI unexpectedly waited for input");
        }
    }
    for (const bool read_error : {false, true}) {
        std::ostringstream output;
        std::ostringstream error;
        const Console console{detail::make_ftxui_console_backend(
            {.ui = UiMode::tui, .view = TuiView::health},
            output, error, 80, 20
        )};
        std::istringstream input;
        auto* original = std::cin.rdbuf(input.rdbuf());
        if (read_error) {
            std::cin.setstate(std::ios::badbit);
        }
        const bool threw = onedrive::test::throws_with<std::runtime_error>(
            [&console] { console.finish(); },
            "cannot read terminal completion input"
        );
        const bool reached_eof = std::cin.eof();
        std::cin.rdbuf(original);
        if (threw != read_error || (!read_error && !reached_eof)) {
            return fail("inspection completion mishandled EOF or input failure");
        }
    }
    for (const auto mode : {OutputMode::text, OutputMode::json}) {
        std::ostringstream output;
        std::ostringstream error;
        const Console console{
            {.output = mode, .ui = UiMode::console, .view = TuiView::health},
            output, error
        };
        std::istringstream input{"untouched\n"};
        auto* original = std::cin.rdbuf(input.rdbuf());
        console.finish();
        std::cin.rdbuf(original);
        if (input.peek() != 'u' || !output.str().empty() ||
            !error.str().empty()) {
            return fail("console or JSON completion was not a no-op");
        }
    }
    return EXIT_SUCCESS;
}

int test_drive_details(std::size_t rows, int pages) {
    using namespace onedrive::cli;
    std::ostringstream output;
    std::ostringstream error;
    const Console console{detail::make_ftxui_console_backend(
        {.color = ColorMode::never, .ui = UiMode::tui, .view = TuiView::drives},
        output,
        error,
        100,
        rows
    )};
    auto fields = onedrive::app::detail::drive_fields(
        {
            .id = "real-drive-id",
            .name = "OneDrive",
            .quota =
                onedrive::graph::DriveQuota{
                    .total = 5ULL * 1024 * 1024 * 1024,
                    .used = 1024,
                    .remaining = 4096,
                    .deleted = 0,
                    .state = "normal",
                },
        },
        true
    );
    fields.insert(
        fields.begin() + 2,
        {
            .label = "reference:",
            .key = "reference",
            .value = "me",
        }
    );
    fields.insert(
        fields.end(),
        {
            {.label = "statistics source:",
             .key = "statistics_source",
             .value = "local state (not cloud totals)"},
            {.label = "known files:", .key = "known_files", .value = "12"},
            {.label = "downloaded (local):",
             .key = "downloaded_files",
             .value = "8"},
            {.label = "pending downloads:",
             .key = "pending_files",
             .value = "3"},
            {.label = "blocked files:", .key = "blocked_files", .value = "1"},
            {.label = "state database:",
             .key = "state_database",
             .value = "present"},
        }
    );
    for (int page = 0; page < pages; ++page) {
        console.section("drive", "OneDrive drive:", fields);
    }
    std::istringstream input{"\n"};
    auto* original = std::cin.rdbuf(input.rdbuf());
    console.finish();
    std::cin.rdbuf(original);
    const auto rendered = output.str();
    const auto frame = rendered.substr(rendered.rfind("\033[H"));
    if (!frame.contains("Press Enter to exit") || frame.contains("CLOSE") ||
        !frame.contains("Drive 1/" + std::to_string(pages)) ||
        frame.contains("Left/Right or n/p") != (pages > 1)) {
        return fail("drive TUI hid or retained the ambiguous exit prompt");
    }
    if (rows < 30) {
        return EXIT_SUCCESS;
    }
    for (const auto* expected : {
             "reference: me",
             "id: real-drive-id",
             "total: 5.00 GiB",
             "used: 1.00 KiB",
             "remaining: 4.00 KiB",
             "deleted: 0 B",
             "known files: 12",
             "downloaded (local): 8",
             "pending downloads: 3",
             "blocked files: 1",
             "local state (not cloud totals)",
             "Press Enter to exit",
         }) {
        if (!frame.contains(expected)) {
            return fail(std::string{"drive TUI omitted: "} + expected);
        }
    }
    if (frame.contains("CLOSE")) {
        return fail("drive TUI retained the ambiguous exit prompt");
    }
    return EXIT_SUCCESS;
}

int test_drive_pagination() {
    using namespace onedrive::cli;
    struct Case {
        std::string_view input;
        int page;
    };
    constexpr std::array cases{
        Case{"\n", 1},
        Case{"q\n", 1},
        Case{"Q\n", 1},
        Case{"p\nq\n", 1},
        Case{"n\nq\n", 2},
        Case{"n\nn\nn\nq\n", 3},
        Case{"n\np\np\nq\n", 1},
        Case{"\033[C\nq\n", 2},
        Case{"n\n\033[D\nq\n", 1},
        Case{"x\nq\n", 1},
        Case{"", 1},
        Case{"\033[D\nq\n", 1},
        Case{"n\nn\n\033[C\nq\n", 3},
        Case{"q\nuntouched\n", 1},
    };
    for (const auto& test : cases) {
        std::ostringstream output;
        std::ostringstream error;
        const Console console{detail::make_ftxui_console_backend(
            {.color = ColorMode::never,
             .ui = UiMode::tui,
             .view = TuiView::drives},
            output,
            error,
            100,
            12
        )};
        console.section("account", "Account:", {});
        for (int page = 1; page <= 3; ++page) {
            console.section(
                "drive",
                "OneDrive drive:",
                {
                    {.label = "name:",
                     .key = "name",
                     .value = "Drive-name-" + std::to_string(page)},
                    {.label = "id:",
                     .key = "id",
                     .value = std::to_string(page)},
                }
            );
        }
        std::istringstream input{std::string{test.input}};
        auto* original = std::cin.rdbuf(input.rdbuf());
        console.finish();
        console.finish();
        std::cin.rdbuf(original);
        if (test.input.ends_with("untouched\n") && input.peek() != 'u') {
            return fail("Drive paging read input after completion");
        }
        const auto rendered = output.str();
        const auto frame = rendered.substr(rendered.rfind("\033[H"));
        if (!frame.contains("Drive " + std::to_string(test.page) + "/3") ||
            !frame.contains("Drive-name-" + std::to_string(test.page)) ||
            !frame.contains("Left/Right or n/p") ||
            !frame.contains("Press Enter to exit (or q)")) {
            return fail(
                "Drive paging lost results or selected an incorrect page"
            );
        }
        for (int page = 1; page <= 3; ++page) {
            if (page != test.page &&
                frame.contains("Drive-name-" + std::to_string(page))) {
                return fail("Drive paging mixed details from different drives");
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_partial_list() {
    using namespace onedrive::cli;
    struct Case {
        std::string_view input;
        int selected;
    };
    constexpr std::array cases{
        Case{"\n", 1},
        Case{"k\nq\n", 1},
        Case{"j\nq\n", 2},
        Case{"j\nk\nq\n", 1},
        Case{"\033[B\nq\n", 2},
        Case{"j\n\033[A\nq\n", 1},
        Case{"\033[A\nq\n", 1},
        Case{"n\np\nx\nq\n", 1},
        Case{"Q\n", 1},
        Case{"", 1},
    };
    for (const auto columns : {60U, 100U}) {
        for (const auto rows : {12U, 18U, 24U, 30U}) {
            for (const auto& test : cases) {
                std::ostringstream output;
                std::ostringstream error;
                const Console console{detail::make_ftxui_console_backend(
                    {.color = ColorMode::never,
                     .ui = UiMode::tui,
                     .view = TuiView::partials},
                    output,
                    error,
                    columns,
                    rows
                )};
                onedrive::test::emit_partial_results(console, 20);
                std::istringstream input{std::string{test.input}};
                auto* original = std::cin.rdbuf(input.rdbuf());
                console.finish();
                console.finish();
                std::cin.rdbuf(original);
                const auto rendered = output.str();
                const auto frame = rendered.substr(rendered.rfind("\033[H"));
                const auto path =
                    "folder/partial-" + std::to_string(test.selected) + ".bin";
                if (!frame.contains(
                        "File " + std::to_string(test.selected) + "/20"
                    ) ||
                    !frame.contains("> " + path) ||
                    !frame.contains("recorded: 20") ||
                    !frame.contains("resumable: 10") ||
                    !frame.contains("invalid: 10") ||
                    !frame.contains("actual bytes: 40 B") ||
                    !frame.contains("4 B / 8 B") ||
                    !frame.contains("Up/Down or k/j") ||
                    !frame.contains("Press Enter to exit (or q)")) {
                    return fail(
                        "partial list lost files, summary, selection, or "
                        "controls\n" +
                        frame
                    );
                }
                if (rows >= 18 &&
                    (!frame.contains("destination: /sync/" + path) ||
                     !frame.contains(
                         "temporary file: /sync/" + path + ".partial"
                     ) ||
                     !frame.contains(
                         test.selected == 1 ? "actual: 4 B"
                                            : "actual: unavailable"
                     ))) {
                    return fail(
                        "partial list lost selected file details\n" + frame
                    );
                }
            }
        }
    }
    for (const auto count : {0U, 1U}) {
        std::ostringstream output;
        std::ostringstream error;
        const Console console{detail::make_ftxui_console_backend(
            {.color = ColorMode::never,
             .ui = UiMode::tui,
             .view = TuiView::partials},
            output,
            error,
            100,
            24
        )};
        onedrive::test::emit_partial_results(console, count);
        std::istringstream input{"j\nk\n\033[B\n\033[A\nq\nuntouched\n"};
        auto* original = std::cin.rdbuf(input.rdbuf());
        console.finish();
        console.finish();
        std::cin.rdbuf(original);
        const auto rendered = output.str();
        const auto frame = rendered.substr(rendered.rfind("\033[H"));
        if (!frame.contains("recorded: " + std::to_string(count)) ||
            frame.contains("Up/Down or k/j") || input.peek() != 'u' ||
            !frame.contains(
                count == 0 ? "No partial downloads are recorded." : "File 1/1"
            )) {
            return fail(
                "empty or single partial list mishandled navigation\n" + frame
            );
        }
    }
    for (const auto rows : {12U, 18U, 30U}) {
        std::ostringstream output;
        std::ostringstream error;
        const Console console{detail::make_ftxui_console_backend(
            {.color = ColorMode::never,
             .ui = UiMode::tui,
             .view = TuiView::partials},
            output,
            error,
            60,
            rows
        )};
        onedrive::test::emit_partial_results(
            console, 3, std::string(200, 'x') + "/"
        );
        std::istringstream input{"j\nq\n"};
        auto* original = std::cin.rdbuf(input.rdbuf());
        console.finish();
        std::cin.rdbuf(original);
        const auto rendered = output.str();
        const auto frame = rendered.substr(rendered.rfind("\033[H"));
        if (!frame.contains("File 2/3") || !frame.contains("4 B / 8 B") ||
            !frame.contains("Up/Down or k/j") ||
            !frame.contains("Press Enter to exit (or q)")) {
            return fail("long partial paths hid list navigation\n" + frame);
        }
    }
    {
        std::ostringstream output;
        std::ostringstream error;
        const Console console{detail::make_ftxui_console_backend(
            {.ui = UiMode::tui, .view = TuiView::partials},
            output,
            error,
            100,
            24
        )};
        if (!onedrive::test::throws_with<std::runtime_error>(
                [&] {
                    console.section(
                        "partial_download", "Partial download:", {}
                    );
                    console.finish();
                },
                "partial_download result is missing field 'remote_path'"
            )) {
            return fail(
                "partial list silently accepted incomplete result fields"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_drive_field_helpers() {
    using onedrive::app::detail::format_bytes;
    if (format_bytes(0) != "0 B" || format_bytes(1023) != "1023 B" ||
        format_bytes(1024) != "1.00 KiB" ||
        format_bytes(1ULL << 20) != "1.00 MiB" ||
        format_bytes(1ULL << 30) != "1.00 GiB" ||
        format_bytes(1ULL << 40) != "1.00 TiB" ||
        format_bytes(1ULL << 50) != "1024.00 TiB") {
        return fail("shared quota formatter mishandled unit boundaries");
    }
    const auto fields = onedrive::app::detail::drive_fields({
        .id = "site-drive",
        .name = "Library",
        .quota = std::nullopt,
    });
    if (std::ranges::any_of(
            fields,
            [](const auto& field) {
                return field.key == "configured" || field.key == "reference";
            }
        ) ||
        std::ranges::none_of(fields, [](const auto& field) {
            return field.key == "total" && field.value == "unavailable";
        })) {
        return fail("shared Drive fields invented configuration or quota data");
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
    auth_backend->emit(MessageEvent{
        .kind = MessageKind::information,
        .event = "device_authorization",
        .text =
            "To sign in, use a web browser to open the page "
            "https://login.microsoftonline.com/device and enter the code "
            "ABCD-EFGH to authenticate.",
    });
    auth_backend->emit(MessageEvent{
        .kind = MessageKind::information,
        .event = "authorization_wait",
        .text = "Waiting for authorization...",
    });
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
    doctor_backend->emit(SectionEvent{
        .event = "database_integrity",
        .title = "Synchronization state database:",
        .fields = {
            {.label = "status:", .key = "status", .value = "healthy"},
        },
    });
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
    status_backend->emit(SectionEvent{
        .event = "status",
        .title = "Synchronization status:",
        .fields = {
            {.label = "account:", .key = "account", .value = "Test User"},
            {.label = "tracked:", .key = "tracked_items", .value = "42"},
        },
    });
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
            .view = TuiView::watch,
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

}  // namespace

int main() {
    if (const auto result = test_partial_list(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = test_drive_pagination(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = test_drive_field_helpers();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = test_inspection_completion(); result != EXIT_SUCCESS) {
        return result;
    }
    for (const auto rows : {12U, 24U, 30U}) {
        for (const auto pages : {1, 3}) {
            if (const auto result = test_drive_details(rows, pages);
                result != EXIT_SUCCESS) {
                return result;
            }
        }
    }
    return test_tui_dashboard();
}
