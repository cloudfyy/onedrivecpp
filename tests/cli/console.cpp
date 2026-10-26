#include "support/progress.hpp"
#include "onedrive/ui/cli/console.hpp"
#include "support/common.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <algorithm>
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

namespace {

int test_console_events_and_output() {
    using namespace onedrive::cli;
    using onedrive::util::ProgressState;

    for (const auto mode : {OutputMode::text, OutputMode::json}) {
        for (const bool quiet : {false, true}) {
            std::ostringstream output;
            std::ostringstream error;
            const Console console{
                {.output = mode, .quiet = quiet, .file_history_limit = 2},
                output,
                error
            };
            for (int index = 0; index < 3; ++index) {
                console.file_operation({
                    .completed_at =
                        std::chrono::system_clock::time_point{
                            std::chrono::milliseconds{index}
                        },
                    .action = onedrive::events::FileAction::moved_remotely,
                    .path = "file-" + std::to_string(index),
                    .previous_path = "old\nname.txt",
                    .drive_id = "drive",
                });
            }
            console.sync_summary({});
            console.finish();
            if (!error.str().empty() || (quiet && !output.str().empty())) {
                return fail(
                    "file history bypassed quiet mode or wrote to stderr"
                );
            }
            if (quiet) {
                continue;
            }
            if (mode == OutputMode::text) {
                if (!output.str().contains("1970-01-01T00:00:00Z") ||
                    !output.str().contains("moved_remotely") ||
                    !output.str().contains("old\\nname.txt") ||
                    !output.str().contains(
                        "\n\"old\\nname.txt\" -> \"file-0\"  moved_remotely "
                        " 1970-01-01T00:00:00Z\n"
                    ) ||
                    output.str().contains("[drive]") ||
                    !output.str().contains(
                        "Drives (retained history): \"drive\"\n"
                    ) ||
                    !output.str().contains(
                        "2 retained (limit 2); 1 older operations omitted."
                    )) {
                    return fail(
                        "text file history lost time, action, paths or "
                        "omissions"
                    );
                }
            } else {
                std::istringstream lines{output.str()};
                std::string line;
                int count = 0;
                while (std::getline(lines, line)) {
                    const auto event = nlohmann::json::parse(line);
                    if (event["event"] != "file_operation" ||
                        event["action"] != "moved_remotely" ||
                        event["previous_path"] != "old\nname.txt" ||
                        event["drive_id"] != "drive" ||
                        event["completed_at_unix_milliseconds"] != count ||
                        event["history_retained"] != std::min(count + 1, 2) ||
                        event["history_omitted"] != std::max(count - 1, 0)) {
                        return fail(
                            "JSON file operation fields or retention "
                            "accounting changed"
                        );
                    }
                    ++count;
                }
                if (count != 3) {
                    return fail("file operation JSON was duplicated or lost");
                }
            }
        }
    }

    {
        std::ostringstream output;
        std::ostringstream error;
        const Console upload_json{{.output = OutputMode::json}, output, error};
        upload_json.upload_progress(
            {.completed_files = 0,
             .file_count = 2,
             .uploaded = 50,
             .total = 100,
             .metrics = {
                 .bytes_per_second = 25, .estimated_seconds_remaining = 2
             }}
        );
        upload_json.upload_progress(
            {.completed_files = 2,
             .file_count = 2,
             .uploaded = 100,
             .total = 100,
             .state = ProgressState::completed}
        );
        upload_json.upload_progress({});
        upload_json.sync_phase(onedrive::events::SyncPhase::uploading);
        upload_json.sync_context(onedrive::sync::SyncMode::upload_only, false);
        upload_json.end_upload_progress();
        std::istringstream lines{output.str()};
        std::string line;
        std::vector<nlohmann::json> records;
        while (std::getline(lines, line)) {
            records.push_back(nlohmann::json::parse(line));
        }
        if (records.size() != 5 || records[0]["event"] != "upload_progress" ||
            records[0]["uploaded_bytes"] != 50 ||
            records[0]["percentage"] != 50 ||
            records[0]["bytes_per_second"] != 25 ||
            records[0]["estimated_seconds_remaining"] != 2 ||
            records[1]["completed"] != true ||
            !records[2]["percentage"].is_null() ||
            records[3]["phase"] != "uploading" ||
            records[4]["direction"] != "upload_only" || !error.str().empty()) {
            return fail(
                "upload JSON events lost progress, phase, or unknown totals"
            );
        }
        std::ostringstream text;
        const Console upload_text{{}, text, error};
        upload_text.upload_progress(
            {.completed_files = 1,
             .file_count = 2,
             .uploaded = 50,
             .total = 100}
        );
        upload_text.upload_progress(
            {.completed_files = 2,
             .file_count = 2,
             .uploaded = 100,
             .total = 100,
             .state = ProgressState::completed}
        );
        upload_text.upload_progress({});
        if (!text.str().contains("UL: 1/2 files, 50%") ||
            !text.str().contains("Upload done: 2/2 files, 100%") ||
            !text.str().contains(
                "UL: totals unknown; percentage unavailable"
            )) {
            return fail(
                "console upload progress did not preserve direction or totals"
            );
        }
        std::ostringstream quiet_output;
        const Console quiet_upload{
            {.output = OutputMode::json, .quiet = true}, quiet_output, error
        };
        quiet_upload.upload_progress({});
        quiet_upload.sync_phase(onedrive::events::SyncPhase::waiting);
        quiet_upload.sync_context(
            onedrive::sync::SyncMode::bidirectional, true
        );
        if (!quiet_output.str().empty()) {
            return fail("quiet output leaked upload events");
        }
    }

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
    plain.message(MessageKind::information, "completed", "Completed.");
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
        "conflict.txt", "local_modification", "local file was modified"
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
    plain.download_progress(1, 2, 10, 10, ProgressState::ongoing);
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
    plain.download_progress(0, 1, 1'536, 3'072, ProgressState::ongoing);
    plain.download_progress(
        0, 1, 1024U * 1024U, 2U * 1024U * 1024U, ProgressState::ongoing
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
        plain_error.str() != "Blocked 'conflict.txt': local file was modified "
                             "(local_modification)\n") {
        return fail("plain console output was incorrect");
    }
    try {
        plain.message(static_cast<MessageKind>(255), "invalid", "Invalid");
        return fail("text console accepted an unknown message kind");
    } catch (const std::logic_error&) {
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
    styled.message(MessageKind::information, "completed", "Completed.");
    if (!styled_output.str().contains("\033[36m") ||
        !styled_output.str().contains("->") ||
        !styled_output.str().contains("\033[0m")) {
        return fail("forced color output did not contain ANSI styling");
    }

    for (const auto& [kind, level] :
         {std::pair{MessageKind::trace, "trace"},
          std::pair{MessageKind::debug, "debug"},
          std::pair{MessageKind::information, "info"},
          std::pair{MessageKind::warning, "warning"},
          std::pair{MessageKind::error, "error"},
          std::pair{MessageKind::critical, "critical"}}) {
        const bool is_error =
            kind == MessageKind::error || kind == MessageKind::critical;
        std::ostringstream output;
        std::ostringstream error;
        const Console level_json{{.output = OutputMode::json}, output, error};
        level_json.message(kind, "log", level);
        const auto payload =
            nlohmann::json::parse(is_error ? error.str() : output.str());
        if (payload.at("level") != level || payload.at("message") != level ||
            !(is_error ? output.str() : error.str()).empty()) {
            return fail("JSON output changed backend levels or error routing");
        }
        output.str({});
        error.str({});
        const Console level_text{
            {.color = ColorMode::always, .output = OutputMode::text},
            output,
            error
        };
        level_text.message(kind, "log", level);
        if (!(is_error ? error.str() : output.str()).contains(level) ||
            !(is_error ? output.str() : error.str()).empty()) {
            return fail("text output lost a backend level or error routing");
        }
        output.str({});
        error.str({});
        const Console level_quiet{
            {.output = OutputMode::json, .quiet = true}, output, error
        };
        level_quiet.message(kind, "log", level);
        if (kind == MessageKind::trace || kind == MessageKind::debug ||
            kind == MessageKind::information) {
            if (!output.str().empty() || !error.str().empty()) {
                return fail("quiet JSON retained a low-severity message");
            }
        } else if (nlohmann::json::parse(is_error ? error.str() : output.str())
                       .at("level") != level) {
            return fail("quiet JSON suppressed or changed a diagnostic level");
        }
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
        "conflict.txt", "local_modification", "local file was modified"
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
    const auto completed_download_progress = nlohmann::json::parse(line);
    const auto blocked = nlohmann::json::parse(json_error.str());
    if (message.at("event") != "phase" || message.at("message") != "Working" ||
        section.at("event") != "summary" ||
        section.at("values").at("files") != "12" ||
        delta_progress.at("event") != "delta_progress" ||
        delta_progress.at("pages") != 4 || delta_progress.at("items") != 625 ||
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
        completed_download_progress.at("event") != "download_progress" ||
        completed_download_progress.at("completed") != true ||
        completed_download_progress.at("bytes_per_second") != 8 ||
        !completed_download_progress.at("estimated_seconds_remaining")
             .is_null() ||
        completed_download_progress.at("elapsed_milliseconds") != 1'500 ||
        blocked.at("event") != "item_blocked" ||
        blocked.at("path") != "conflict.txt" ||
        blocked.at("reason_code") != "local_modification" ||
        json.output_mode() != OutputMode::json ||
        json_output.str().contains("\033[")) {
        return fail("JSON console output was invalid");
    }
    try {
        json.message(static_cast<MessageKind>(255), "invalid", "Invalid");
        return fail("JSON console accepted an unknown message kind");
    } catch (const std::logic_error&) {
    }
    try {
        static_cast<void>(json.confirm("confirm", "Continue?", "yes"));
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
    quiet_json.message(MessageKind::trace, "trace", "Hidden");
    quiet_json.message(MessageKind::debug, "debug", "Hidden");
    quiet_json.section(
        "summary", "Hidden", {{.label = "Files", .key = "files", .value = "12"}}
    );
    quiet_json.delta_progress(1, 200, ProgressState::ongoing);
    quiet_json.delta_summary({
        .pages = 1,
        .scanned_items = 200,
    });
    quiet_json.download_progress(0, 1, 1, 2, ProgressState::ongoing);
    quiet_json.blocked_item(
        "conflict.txt", "local_modification", "local file was modified"
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
    quiet.message(MessageKind::trace, "trace", "Hidden");
    quiet.message(MessageKind::debug, "debug", "Hidden");
    quiet.section(
        "summary", "Hidden", {{.label = "Files", .key = "files", .value = "12"}}
    );
    quiet.delta_progress(1, 200, ProgressState::ongoing);
    quiet.delta_summary({
        .pages = 1,
        .scanned_items = 200,
    });
    quiet.download_progress(0, 1, 1, 2, ProgressState::ongoing);
    quiet.blocked_item(
        "conflict.txt", "local_modification", "local file was modified"
    );
    quiet.message(MessageKind::warning, "warning", "Visible warning");
    quiet.message(MessageKind::error, "error", "Visible error");
    quiet.message(MessageKind::critical, "critical", "Visible critical");
    if (quiet_output.str() != "Visible warning\n" ||
        quiet_error.str() !=
            "Blocked 'conflict.txt': local file was modified "
            "(local_modification)\nVisible error\nVisible critical\n") {
        return fail("quiet mode suppressed or retained the wrong output");
    }
    if (plain.output_mode() != OutputMode::text) {
        return fail("text console reported the wrong output mode");
    }

    if (download_progress_percentage(0, 0, 0, 0, ProgressState::ongoing) != 0 ||
        download_progress_percentage(0, 0, 0, 0, ProgressState::completed) !=
            100 ||
        download_progress_percentage(1, 4, 0, 0, ProgressState::ongoing) !=
            25 ||
        download_progress_percentage(5, 4, 0, 0, ProgressState::ongoing) !=
            99) {
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
    automatic.message(MessageKind::information, "phase", "Console fallback");
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
        Console::parse_ui_mode("tui") != UiMode::tui ||
        Console::parse_tui_theme("hacker") != TuiTheme::hacker ||
        Console::parse_tui_theme("ocean") != TuiTheme::ocean ||
        Console::parse_tui_theme("amber") != TuiTheme::amber ||
        Console::parse_tui_theme("synthwave") != TuiTheme::synthwave ||
        Console::parse_language("auto") != Language::automatic ||
        Console::parse_language("en") != Language::english ||
        Console::parse_language("zh_CN") != Language::simplified_chinese ||
        Console::parse_language("zh-CN") != Language::simplified_chinese ||
        Console::parse_language("zh") != Language::simplified_chinese) {
        return fail("console option parsing returned the wrong mode");
    }
    try {
        static_cast<void>(Console::parse_language("invalid"));
        return fail("invalid language was accepted");
    } catch (const std::invalid_argument&) {
    }

    std::ostringstream zh_output;
    std::ostringstream zh_error;
    const Console zh_console{
        {
            .color = ColorMode::never,
            .output = OutputMode::text,
            .language = Language::simplified_chinese,
            .file_history_limit = 2,
        },
        zh_output,
        zh_error
    };
    if (zh_console.language() != Language::simplified_chinese) {
        return fail("Chinese console did not report simplified_chinese");
    }
    zh_console.section(
        "effective_config",
        "Effective configuration:",
        {
            {.label = "language:", .key = "language", .value = "zh_CN"},
            {.label = "sync mode:", .key = "sync_mode", .value = "bidirectional"},
        }
    );
    zh_console.sync_context(onedrive::sync::SyncMode::bidirectional, true);
    zh_console.sync_phase(onedrive::events::SyncPhase::scanning_local);
    zh_console.delta_progress(2, 5, ProgressState::ongoing);
    zh_console.delta_summary({
        .pages = 2,
        .scanned_items = 5,
        .unique_changes = 4,
        .files = 3,
        .directories = 1,
        .deletions = 0,
    });
    zh_console.download_progress(
        1,
        2,
        512,
        1024,
        ProgressState::ongoing,
        {.bytes_per_second = 256, .estimated_seconds_remaining = 2}
    );
    zh_console.download_progress(
        2,
        2,
        1024,
        1024,
        ProgressState::completed,
        {.bytes_per_second = 512, .elapsed_milliseconds = 500}
    );
    zh_console.upload_progress({});
    zh_console.upload_progress(
        {.completed_files = 1,
         .file_count = 1,
         .uploaded = 1024,
         .total = 1024,
         .state = ProgressState::completed,
         .metrics = {.bytes_per_second = 1024, .elapsed_milliseconds = 2000}}
    );
    zh_console.message(
        MessageKind::information, "sync_completed", "Sync complete."
    );
    zh_console.message(
        MessageKind::information,
        "monitor_ready",
        "Monitoring local and Microsoft Graph changes for:\n  /tmp/sync"
    );
    zh_console.message(
        MessageKind::information,
        "device_authorization",
        "Open https://microsoft.com/deviceloginchina and enter code ABCD-EFGH"
    );
    zh_console.message(
        MessageKind::warning,
        "cursor_reset",
        "Reset synchronization cursor for drive 'me': saved cursor removed."
    );
    zh_console.file_operation({
        .completed_at = std::chrono::system_clock::time_point{},
        .action = onedrive::events::FileAction::downloaded,
        .path = "folder/new.txt",
        .previous_path = "folder/old.txt",
        .drive_id = "drive-zh",
        .local_path = "/tmp/sync/folder/new.txt",
        .directory = true,
        .recovered = true,
    });
    zh_console.finish();
    zh_console.blocked_item(
        "conflict.txt", "local_modification", "local file was modified"
    );
    zh_console.blocked_item(
        "large_delete",
        "large_delete_guard",
        "Remote deletion plan affects 15 tracked items, exceeding the "
        "configured limit of 10."
    );
    {
        std::istringstream confirm_in{"me\n"};
        auto* old_cin = std::cin.rdbuf(confirm_in.rdbuf());
        const bool confirmed = zh_console.confirm(
            "clear_state",
            "Type the configured drive reference 'me' to confirm: ",
            "me"
        );
        std::cin.rdbuf(old_cin);
        if (!confirmed) {
            return fail("Chinese console confirmation failed");
        }
    }
    if (!zh_output.str().contains(
            "有效配置：\n  语言：     简体中文\n  同步模式： bidirectional\n"
        ) ||
        !zh_output.str().contains("同步：双向同步（预览）\n") ||
        !zh_output.str().contains("阶段：扫描本地更改\n") ||
        !zh_output.str().contains("Microsoft Graph 增量同步：..\n") ||
        !zh_output.str().contains("Microsoft Graph 增量同步完成：") ||
        !zh_output.str().contains(
            "下载：1/2 个文件，50%（512 B/1.0 KiB），256 B/s，预计剩余 00:00:02\n"
        ) ||
        !zh_output.str().contains(
            "完成：2/2 个文件，100%（1.0 KiB/1.0 KiB），512 B/s，用时 <1s\n"
        ) ||
        !zh_output.str().contains("上传：总量未知；无法计算百分比\n") ||
        !zh_output.str().contains(
            "上传完成：1/1 个文件，100%（1.0 KiB/1.0 KiB），1.0 KiB/s，用时 00:00:02\n"
        ) ||
        !zh_output.str().contains("同步完成。\n") ||
        !zh_output.str().contains(
            "正在监控本地与 Microsoft Graph 远端更改：\n  /tmp/sync\n"
        ) ||
        !zh_output.str().contains(
            "请打开 https://microsoft.com/deviceloginchina 并输入验证码 ABCD-EFGH\n"
        ) ||
        !zh_output.str().contains(
            "已重置驱动器 'me' 的同步游标：已保存游标已移除。\n"
        ) ||
        !zh_output.str().contains("已完成的文件操作（当前进程）：\n") ||
        !zh_output.str().contains(
            "\"folder/old.txt\" -> \"folder/new.txt\"  已下载（文件夹）（已恢复） "
            "[本地：\"/tmp/sync/folder/new.txt\"]  1970-01-01T00:00:00Z\n"
        ) ||
        !zh_output.str().contains(
            "文件历史记录：保留 1 条（上限 2）；已省略 0 条更早的操作。\n"
            "驱动器（保留的历史记录）：\"drive-zh\"\n"
        ) ||
        !zh_output.str().contains("请输入配置的驱动器引用 'me' 以确认：") ||
        !zh_error.str().contains(
            "已受阻 'conflict.txt'：local file was modified (local_modification)"
        ) ||
        !zh_error.str().contains(
            "已受阻 'large_delete'：远端删除计划涉及 15 个受跟踪项目，超过配置上限 10。 "
            "(large_delete_guard)"
        )) {
        return fail(
            "Chinese text console did not localize or align output:\n" +
            zh_output.str() + zh_error.str()
        );
    }

    {
        std::ostringstream fallback_output;
        std::ostringstream fallback_error;
        const Console fallback_console{
            {
                .color = ColorMode::never,
                .output = OutputMode::text,
                .language = static_cast<Language>(99),
            },
            fallback_output,
            fallback_error
        };
        fallback_console.sync_context(
            onedrive::sync::SyncMode::upload_only, false
        );
        fallback_console.message(
            MessageKind::information, "sync_completed", "Sync complete."
        );
        if (!fallback_output.str().contains("Synchronization: upload_only\n") ||
            !fallback_output.str().contains("Sync complete.\n")) {
            return fail(
                "unregistered language value did not fall back to English "
                "catalog:\n" +
                fallback_output.str()
            );
        }
    }
    try {
        static_cast<void>(Console::parse_color_mode("invalid"));
        return fail("invalid color mode was accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(Console::parse_tui_theme("invalid"));
        return fail("invalid TUI theme was accepted");
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
    event_console.message(MessageKind::information, "phase", "Working");
    event_console.section(
        "summary", "Summary", {{.label = "Files", .key = "files", .value = "2"}}
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
    event_console.blocked_item("conflict.txt", "local_modification", "changed");
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
        !std::holds_alternative<DeltaProgressEvent>(captured->events[2]) ||
        !std::holds_alternative<DeltaSummaryEvent>(captured->events[3]) ||
        !std::holds_alternative<BlockedItemEvent>(captured->events[4]) ||
        !std::holds_alternative<DownloadProgressEvent>(captured->events[5]) ||
        !std::holds_alternative<EndDownloadProgressEvent>(
            captured->events[6]
        ) ||
        !std::holds_alternative<MessageEvent>(captured->events[7]) ||
        std::get<MessageEvent>(captured->events[0]).event != "phase" ||
        std::get<DeltaSummaryEvent>(captured->events[3]).summary.files != 300 ||
        std::get<DownloadProgressEvent>(captured->events[5])
                .metrics.bytes_per_second != 5 ||
        std::get<MessageEvent>(captured->events[7]).kind !=
            MessageKind::warning ||
        !captured->confirmation || captured->confirmation->event != "confirm" ||
        captured->confirmation->expected != "yes") {
        return fail("console facade did not publish structured events");
    }
    try {
        const Console invalid{std::unique_ptr<ConsoleBackend>{}};
        return fail("null console backend was accepted");
    } catch (const std::invalid_argument&) {
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    onedrive::test::ScenarioProgress progress{1};
    if (const auto result = progress.run(
            "console_events_and_output", test_console_events_and_output
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    return progress.finish();
}
