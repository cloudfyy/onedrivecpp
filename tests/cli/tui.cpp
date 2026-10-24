#include "support/progress.hpp"
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

using onedrive::test::fail;
using namespace onedrive::test::tui_cases;

int main() {
    onedrive::test::ScenarioProgress progress{16};
    {
        using namespace onedrive::cli;
        std::ostringstream output;
        std::ostringstream error;
        const Console console{detail::make_ftxui_console_backend(
            {.ui = UiMode::tui, .view = TuiView::watch}, output, error, 120, 40
        )};
        const int completed_round =
            progress.run("monitor_round_completion", [&] {
                console.operation_state(
                    OperationKind::monitoring, OperationState::watching
                );
                console.sync_context(
                    onedrive::sync::SyncMode::bidirectional, false
                );
                console.sync_phase(onedrive::events::SyncPhase::preparing);
                console.download_progress(
                    1, 1, 10, 10, onedrive::util::ProgressState::completed
                );
                console.upload_progress(
                    {.completed_files = 0,
                     .file_count = 1,
                     .uploaded = 5,
                     .total = 10}
                );
                console.sync_summary(
                    {.downloaded = 1, .uploaded = 0, .blocked = 1}
                );
                console.sync_phase(onedrive::events::SyncPhase::waiting);
                const auto rendered = output.str();
                const auto frame = rendered.substr(rendered.rfind("\033[H"));
                if (frame.contains("DOWNLOADS") || frame.contains("UPLOADS") ||
                    !frame.contains("automatic monitoring") ||
                    !frame.contains("MONITORING: waiting") ||
                    !frame.contains("Last synchronization round incomplete.")) {
                    return fail(
                        "TUI retained transfer panels or stopped monitoring "
                        "after a round"
                    );
                }
                return EXIT_SUCCESS;
            });
        if (completed_round != EXIT_SUCCESS) {
            return completed_round;
        }
        if (const auto result =
                progress.run("adaptive_sync_layout", test_adaptive_sync_layout);
            result != EXIT_SUCCESS) {
            return result;
        }
        const int next_round = progress.run("monitor_round_restart", [&] {
            console.sync_phase(onedrive::events::SyncPhase::preparing);
            console.download_progress(
                0, 0, 0, 0, onedrive::util::ProgressState::ongoing
            );
            console.upload_progress({});
            console.end_upload_progress();
            const auto unknown =
                output.str().substr(output.str().rfind("\033[H"));
            const std::string unavailable{
                "Totals unknown; percentage unavailable."
            };
            const auto first = unknown.find(unavailable);
            if (!unknown.contains("DOWNLOADS") ||
                !unknown.contains("UPLOADS") || first == std::string::npos ||
                unknown.find(unavailable, first + unavailable.size()) ==
                    std::string::npos ||
                unknown.contains("100%")) {
                return fail(
                    "TUI invented completion for unknown transfer totals"
                );
            }
            console.sync_phase(onedrive::events::SyncPhase::preparing);
            const auto next = output.str().substr(output.str().rfind("\033[H"));
            if (next.contains("UPLOADS") || next.contains("DOWNLOADS") ||
                next.contains(unavailable)) {
                return fail("a new TUI round retained stale transfer progress");
            }
            return EXIT_SUCCESS;
        });
        if (next_round != EXIT_SUCCESS) {
            return next_round;
        }
    }
    if (const auto result = progress.run("message_levels", test_message_levels);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = progress.run("partial_list", test_partial_list);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result =
            progress.run("drive_pagination", test_drive_pagination);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result =
            progress.run("drive_field_helpers", test_drive_field_helpers);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result =
            progress.run("inspection_completion", test_inspection_completion);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const auto result = progress.run("file_history", test_file_history);
        result != EXIT_SUCCESS) {
        return result;
    }
    for (const auto rows : {12U, 24U, 30U}) {
        for (const auto pages : {1, 3}) {
            if (const auto result = progress.run(
                    std::string{"drive_details (rows="} + std::to_string(rows) +
                        ", pages=" + std::to_string(pages) + ")",
                    [&] { return test_drive_details(rows, pages); }
                );
                result != EXIT_SUCCESS) {
                return result;
            }
        }
    }
    return progress.finish(progress.run("tui_dashboard", test_tui_dashboard));
}
