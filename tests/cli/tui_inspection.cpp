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

int test_file_history() {
    using namespace onedrive::cli;
    const auto drive_id = "b!" + std::string(100, 'D');
    for (const auto view : {TuiView::sync, TuiView::watch, TuiView::download}) {
        std::ostringstream output;
        std::ostringstream error;
        const Console console{detail::make_ftxui_console_backend(
            {.ui = UiMode::tui, .view = view, .file_history_limit = 2},
            output,
            error,
            80,
            30
        )};
        for (int index = 0; index < 4; ++index) {
            console.file_operation({
                .completed_at = std::chrono::system_clock::time_point{},
                .action = onedrive::events::FileAction::downloaded,
                .path = "completed-" + std::to_string(index) + ".txt",
                .drive_id = drive_id,
            });
        }
        if (!output.str().contains("FILES // 2 retained // 2 older omitted")) {
            return fail("live TUI did not display retained file history");
        }
        const auto rendered = output.str();
        const auto frame = rendered.substr(rendered.rfind("\033[H"));
        if (!frame.contains("completed-2.txt") ||
            !frame.contains("completed-3.txt")) {
            return fail("long Drive ID displaced file paths in a narrow TUI");
        }
        const auto before_finish = output.str().size();
        if (view == TuiView::watch) {
            console.finish_file_history();
        } else {
            console.finish();
        }
        const auto final_list = output.str().substr(before_finish);
        if (!final_list.contains("\033[?1049l") ||
            !final_list.contains("1970-01-01T00:00:00Z") ||
            !final_list.contains("downloaded") ||
            !final_list.contains("completed-2.txt") ||
            !final_list.contains("completed-3.txt") ||
            !final_list.contains("[" + drive_id + "]") ||
            final_list.contains("completed-0.txt") ||
            final_list.contains("completed-1.txt") ||
            !final_list.contains("2 older operations omitted")) {
            return fail("TUI exit did not preserve the bounded file list");
        }
        const auto after_finish = output.str().size();
        console.finish();
        if (output.str().size() != after_finish) {
            return fail("TUI file history was printed twice on completion");
        }
    }
    return EXIT_SUCCESS;
}

int test_inspection_completion() {
    using namespace onedrive::cli;
    constexpr std::array inspection_views{
        TuiView::health,
        TuiView::status,
        TuiView::blocked,
        TuiView::drives,
        TuiView::shared,
        TuiView::sites,
        TuiView::quota,
        TuiView::storage,
        TuiView::partials,
        TuiView::files,
        TuiView::verify,
        TuiView::config,
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
                    MessageKind::information,
                    "result",
                    "Inspection result retained"
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
            {.ui = UiMode::tui, .view = TuiView::health}, output, error, 80, 20
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
            return fail(
                "inspection completion mishandled EOF or input failure"
            );
        }
    }
    for (const auto mode : {OutputMode::text, OutputMode::json}) {
        std::ostringstream output;
        std::ostringstream error;
        const Console console{
            {.output = mode, .ui = UiMode::console, .view = TuiView::health},
            output,
            error
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
                    !frame.contains("temporary files total: 40 B") ||
                    !frame.contains("SAVED PROGRESS / TOTAL") ||
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
                         test.selected == 1
                             ? "local temporary file size: 4 B"
                             : "local temporary file size: unavailable"
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

} // namespace onedrive::test::tui_cases
