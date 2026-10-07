#include "onedrive/cli/console.hpp"
#include "support/common.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using onedrive::test::fail;

class CapturingBackend final : public onedrive::cli::ConsoleBackend {
public:
    void emit(const onedrive::cli::ConsoleEvent& event) override {
        events.push_back(event);
    }

    bool confirm(
        const onedrive::cli::ConfirmationRequest& request
    ) override {
        confirmation = request;
        return confirmation_result;
    }

    onedrive::cli::OutputMode output_mode() const noexcept override {
        return onedrive::cli::OutputMode::json;
    }

    onedrive::cli::UiMode ui_mode() const noexcept override {
        return onedrive::cli::UiMode::console;
    }

    std::vector<onedrive::cli::ConsoleEvent> events;
    std::optional<onedrive::cli::ConfirmationRequest> confirmation;
    bool confirmation_result{false};
};

}  // namespace

int main() {
    using namespace onedrive::cli;
    using onedrive::util::ProgressState;

    std::ostringstream plain_output;
    std::ostringstream plain_error;
    const Console plain{
        {
            .color = ColorMode::never,
            .output = OutputMode::text,
        },
        plain_output,
        plain_error
    };
    plain.message(MessageKind::success, "completed", "Completed.");
    plain.section(
        "summary",
        "Summary",
        {
            {.label = "Files:", .key = "files", .value = "12"},
            {.label = "Bytes:", .key = "bytes", .value = "42"},
        }
    );
    plain.delta_progress(2, 350, ProgressState::ongoing);
    plain.delta_progress(3, 412, ProgressState::completed);
    plain.delta_summary({
        .pages = 3,
        .scanned_items = 412,
        .unique_changes = 400,
        .files = 300,
        .directories = 90,
        .deletions = 10,
    });
    plain.blocked_item(
        "conflict.txt",
        "local_modification",
        "local file was modified"
    );
    plain.download_progress(
        1,
        2,
        5,
        10,
        ProgressState::ongoing,
        {
            .bytes_per_second = 2'048,
            .estimated_seconds_remaining = 65,
            .elapsed_milliseconds = 500,
        }
    );
    plain.download_progress(
        1, 2, 10, 10, ProgressState::ongoing
    );
    plain.download_progress(
        2,
        2,
        10,
        10,
        ProgressState::completed,
        {
            .bytes_per_second = 1'024,
            .elapsed_milliseconds = 3'723'000,
        }
    );
    plain.download_progress(
        0, 1, 1'536, 3'072, ProgressState::ongoing
    );
    plain.download_progress(
        0,
        1,
        1024U * 1024U,
        2U * 1024U * 1024U,
        ProgressState::ongoing
    );
    plain.download_progress(
        0,
        1,
        1024ULL * 1024ULL * 1024ULL,
        2ULL * 1024ULL * 1024ULL * 1024ULL,
        ProgressState::ongoing
    );
    if (plain_output.str() !=
        "Completed.\nSummary\n  Files: 12\n  Bytes: 42\n"
        "Microsoft Graph delta: ...\n"
        "Microsoft Graph delta complete: 3 pages, 412 items scanned, 400 "
        "unique changes (300 files, 90 folders, 10 deletions), 137.3 "
        "items/page\n"
        "DL: 1/2 files, 50% (5 B/10 B), 2.0 KiB/s, ETA 00:01:05\n"
        "DL: 1/2 files, 99% (10 B/10 B)\n"
        "Done: 2/2 files, 100% (10 B/10 B), 1.0 KiB/s, "
        "elapsed 01:02:03\n"
        "DL: 0/1 files, 50% (1.5 KiB/3.0 KiB)\n"
        "DL: 0/1 files, 50% (1.0 MiB/2.0 MiB)\n"
        "DL: 0/1 files, 50% (1.0 GiB/2.0 GiB)\n" ||
        plain_error.str() !=
            "Blocked 'conflict.txt': local file was modified "
            "(local_modification)\n") {
        return fail("plain console output was incorrect");
    }

    std::ostringstream styled_output;
    std::ostringstream styled_error;
    const Console styled{
        {
            .color = ColorMode::always,
            .output = OutputMode::text,
        },
        styled_output,
        styled_error
    };
    styled.message(MessageKind::success, "completed", "Completed.");
    if (!styled_output.str().contains("\033[32m") ||
        !styled_output.str().contains("OK") ||
        !styled_output.str().contains("\033[0m")) {
        return fail("forced color output did not contain ANSI styling");
    }

    std::ostringstream json_output;
    std::ostringstream json_error;
    const Console json{
        {
            .color = ColorMode::always,
            .output = OutputMode::json,
        },
        json_output,
        json_error
    };
    json.message(MessageKind::information, "phase", "Working");
    json.section(
        "summary",
        "Ignored in JSON",
        {
            {.label = "Files", .key = "files", .value = "12"},
        }
    );
    json.delta_progress(4, 625, ProgressState::ongoing);
    json.delta_summary({
        .pages = 4,
        .scanned_items = 625,
        .unique_changes = 600,
        .files = 500,
        .directories = 90,
        .deletions = 10,
    });
    json.blocked_item(
        "conflict.txt",
        "local_modification",
        "local file was modified"
    );
    json.download_progress(
        0,
        1,
        4,
        8,
        ProgressState::ongoing,
        {
            .bytes_per_second = 4,
            .estimated_seconds_remaining = 1,
            .elapsed_milliseconds = 250,
        }
    );
    json.download_progress(
        1,
        1,
        8,
        8,
        ProgressState::completed,
        {
            .bytes_per_second = 8,
            .elapsed_milliseconds = 1'500,
        }
    );
    json.end_download_progress();
    std::istringstream json_lines{json_output.str()};
    std::string line;
    std::getline(json_lines, line);
    const auto message = nlohmann::json::parse(line);
    std::getline(json_lines, line);
    const auto section = nlohmann::json::parse(line);
    std::getline(json_lines, line);
    const auto delta_progress = nlohmann::json::parse(line);
    std::getline(json_lines, line);
    const auto delta_summary = nlohmann::json::parse(line);
    std::getline(json_lines, line);
    const auto download_progress = nlohmann::json::parse(line);
    std::getline(json_lines, line);
    const auto completed_download_progress =
        nlohmann::json::parse(line);
    const auto blocked = nlohmann::json::parse(json_error.str());
    if (message.at("event") != "phase" ||
        message.at("message") != "Working" ||
        section.at("event") != "summary" ||
        section.at("values").at("files") != "12" ||
        delta_progress.at("event") != "delta_progress" ||
        delta_progress.at("pages") != 4 ||
        delta_progress.at("items") != 625 ||
        delta_progress.at("completed") != false ||
        delta_summary.at("event") != "delta_summary" ||
        delta_summary.at("pages") != 4 ||
        delta_summary.at("scanned_items") != 625 ||
        delta_summary.at("unique_changes") != 600 ||
        delta_summary.at("files") != 500 ||
        delta_summary.at("directories") != 90 ||
        delta_summary.at("deletions") != 10 ||
        download_progress.at("event") != "download_progress" ||
        download_progress.at("completed_files") != 0 ||
        download_progress.at("downloaded_bytes") != 4 ||
        download_progress.at("total_bytes") != 8 ||
        download_progress.at("percentage") != 50 ||
        download_progress.at("bytes_per_second") != 4 ||
        download_progress.at("estimated_seconds_remaining") != 1 ||
        download_progress.at("elapsed_milliseconds") != 250 ||
        download_progress.at("completed") != false ||
        completed_download_progress.at("event") !=
            "download_progress" ||
        completed_download_progress.at("completed") != true ||
        completed_download_progress.at("bytes_per_second") != 8 ||
        !completed_download_progress.at(
            "estimated_seconds_remaining"
        ).is_null() ||
        completed_download_progress.at("elapsed_milliseconds") !=
            1'500 ||
        blocked.at("event") != "item_blocked" ||
        blocked.at("path") != "conflict.txt" ||
        blocked.at("reason_code") != "local_modification" ||
        json.output_mode() != OutputMode::json ||
        json_output.str().contains("\033[")) {
        return fail("JSON console output was invalid");
    }
    try {
        static_cast<void>(
            json.confirm("confirm", "Continue?", "yes")
        );
        return fail("JSON console accepted interactive confirmation");
    } catch (const std::runtime_error&) {
    }

    std::ostringstream quiet_json_output;
    std::ostringstream quiet_json_error;
    const Console quiet_json{
        {
            .output = OutputMode::json,
            .quiet = true,
        },
        quiet_json_output,
        quiet_json_error
    };
    quiet_json.message(MessageKind::information, "info", "Hidden");
    quiet_json.message(MessageKind::success, "success", "Hidden");
    quiet_json.section(
        "summary",
        "Hidden",
        {{.label = "Files", .key = "files", .value = "12"}}
    );
    quiet_json.delta_progress(1, 200, ProgressState::ongoing);
    quiet_json.delta_summary({
        .pages = 1,
        .scanned_items = 200,
    });
    quiet_json.download_progress(
        0, 1, 1, 2, ProgressState::ongoing
    );
    quiet_json.blocked_item(
        "conflict.txt",
        "local_modification",
        "local file was modified"
    );
    const auto quiet_json_blocked =
        nlohmann::json::parse(quiet_json_error.str());
    if (!quiet_json_output.str().empty() ||
        quiet_json_blocked.at("event") != "item_blocked" ||
        quiet_json_blocked.at("path") != "conflict.txt") {
        return fail("quiet JSON mode retained the wrong events");
    }

    std::ostringstream quiet_output;
    std::ostringstream quiet_error;
    const Console quiet{
        {
            .color = ColorMode::never,
            .output = OutputMode::text,
            .quiet = true,
        },
        quiet_output,
        quiet_error
    };
    quiet.message(MessageKind::information, "info", "Hidden");
    quiet.message(MessageKind::success, "success", "Hidden");
    quiet.section(
        "summary",
        "Hidden",
        {{.label = "Files", .key = "files", .value = "12"}}
    );
    quiet.delta_progress(1, 200, ProgressState::ongoing);
    quiet.delta_summary({
        .pages = 1,
        .scanned_items = 200,
    });
    quiet.download_progress(
        0, 1, 1, 2, ProgressState::ongoing
    );
    quiet.blocked_item(
        "conflict.txt",
        "local_modification",
        "local file was modified"
    );
    quiet.message(MessageKind::warning, "warning", "Visible warning");
    quiet.message(MessageKind::error, "error", "Visible error");
    if (quiet_output.str() != "Visible warning\n" ||
        quiet_error.str() !=
            "Blocked 'conflict.txt': local file was modified "
            "(local_modification)\nVisible error\n") {
        return fail("quiet mode suppressed or retained the wrong output");
    }
    if (plain.output_mode() != OutputMode::text) {
        return fail("text console reported the wrong output mode");
    }

    if (download_progress_percentage(
            0, 0, 0, 0, ProgressState::ongoing
        ) != 0 ||
        download_progress_percentage(
            0, 0, 0, 0, ProgressState::completed
        ) != 100 ||
        download_progress_percentage(
            1, 4, 0, 0, ProgressState::ongoing
        ) != 25 ||
        download_progress_percentage(
            5, 4, 0, 0, ProgressState::ongoing
        ) != 99) {
        return fail("download progress fallback percentage was incorrect");
    }

    std::ostringstream automatic_output;
    std::ostringstream automatic_error;
    const Console automatic{
        {
            .ui = UiMode::automatic,
        },
        automatic_output,
        automatic_error
    };
    automatic.message(
        MessageKind::information, "phase", "Console fallback"
    );
    if (automatic_output.str() != "Console fallback\n" ||
        !automatic_error.str().empty()) {
        return fail("automatic UI did not fall back for redirected output");
    }
    try {
        const Console forced_tui{
            {
                .ui = UiMode::tui,
            },
            automatic_output,
            automatic_error
        };
        return fail("forced TUI accepted redirected output");
    } catch (const std::runtime_error&) {
    }
    try {
        const Console json_tui{
            {
                .output = OutputMode::json,
                .ui = UiMode::tui,
            },
            automatic_output,
            automatic_error
        };
        return fail("forced TUI accepted JSON output");
    } catch (const std::invalid_argument&) {
    }
    try {
        const Console quiet_tui{
            {
                .ui = UiMode::tui,
                .quiet = true,
            },
            automatic_output,
            automatic_error
        };
        return fail("forced TUI accepted quiet output");
    } catch (const std::invalid_argument&) {
    }

    if (Console::parse_color_mode("auto") != ColorMode::automatic ||
        Console::parse_color_mode("always") != ColorMode::always ||
        Console::parse_color_mode("never") != ColorMode::never ||
        Console::parse_output_mode("text") != OutputMode::text ||
        Console::parse_output_mode("json") != OutputMode::json ||
        Console::parse_ui_mode("auto") != UiMode::automatic ||
        Console::parse_ui_mode("console") != UiMode::console ||
        Console::parse_ui_mode("tui") != UiMode::tui) {
        return fail("console option parsing returned the wrong mode");
    }
    try {
        static_cast<void>(Console::parse_color_mode("invalid"));
        return fail("invalid color mode was accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(Console::parse_ui_mode("invalid"));
        return fail("invalid UI mode was accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(Console::parse_output_mode("invalid"));
        return fail("invalid output mode was accepted");
    } catch (const std::invalid_argument&) {
    }

    auto backend = std::make_unique<CapturingBackend>();
    auto* captured = backend.get();
    const Console event_console{std::move(backend)};
    event_console.message(
        MessageKind::information, "phase", "Working"
    );
    event_console.section(
        "summary",
        "Summary",
        {{.label = "Files", .key = "files", .value = "2"}}
    );
    event_console.delta_progress(2, 350, ProgressState::ongoing);
    event_console.delta_summary({
        .pages = 2,
        .scanned_items = 350,
        .unique_changes = 340,
        .files = 300,
        .directories = 30,
        .deletions = 10,
    });
    event_console.blocked_item(
        "conflict.txt", "local_modification", "changed"
    );
    event_console.download_progress(
        1,
        2,
        5,
        10,
        ProgressState::ongoing,
        {
            .bytes_per_second = 5,
            .estimated_seconds_remaining = 1,
            .elapsed_milliseconds = 1'000,
        }
    );
    event_console.end_download_progress();
    if (event_console.confirm("confirm", "Continue?", "yes")) {
        return fail("capturing backend confirmation unexpectedly matched");
    }
    if (event_console.output_mode() != OutputMode::json ||
        event_console.ui_mode() != UiMode::console ||
        captured->events.size() != 8 ||
        !std::holds_alternative<MessageEvent>(captured->events[0]) ||
        !std::holds_alternative<SectionEvent>(captured->events[1]) ||
        !std::holds_alternative<DeltaProgressEvent>(
            captured->events[2]
        ) ||
        !std::holds_alternative<DeltaSummaryEvent>(
            captured->events[3]
        ) ||
        !std::holds_alternative<BlockedItemEvent>(
            captured->events[4]
        ) ||
        !std::holds_alternative<DownloadProgressEvent>(
            captured->events[5]
        ) ||
        !std::holds_alternative<EndDownloadProgressEvent>(
            captured->events[6]
        ) ||
        !std::holds_alternative<MessageEvent>(captured->events[7]) ||
        std::get<MessageEvent>(captured->events[0]).event != "phase" ||
        std::get<DeltaSummaryEvent>(captured->events[3])
                .summary.files != 300 ||
        std::get<DownloadProgressEvent>(captured->events[5])
                .metrics.bytes_per_second != 5 ||
        std::get<MessageEvent>(captured->events[7]).kind !=
            MessageKind::warning ||
        !captured->confirmation ||
        captured->confirmation->event != "confirm" ||
        captured->confirmation->expected != "yes") {
        return fail("console facade did not publish structured events");
    }
    try {
        const Console invalid{
            std::unique_ptr<ConsoleBackend>{}
        };
        return fail("null console backend was accepted");
    } catch (const std::invalid_argument&) {
    }
    return EXIT_SUCCESS;
}
