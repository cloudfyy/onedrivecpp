#include "onedrive/cli/console.hpp"
#include "test_support.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using onedrive::test::fail;

}  // namespace

int main() {
    using namespace onedrive::cli;

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
    plain.delta_progress(2, 350, false);
    plain.delta_progress(3, 412, true);
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
        false,
        {
            .bytes_per_second = 2'048,
            .estimated_seconds_remaining = 65,
            .elapsed_milliseconds = 500,
        }
    );
    plain.download_progress(1, 2, 10, 10, false);
    plain.download_progress(
        2,
        2,
        10,
        10,
        true,
        {
            .bytes_per_second = 1'024,
            .elapsed_milliseconds = 3'723'000,
        }
    );
    plain.download_progress(0, 1, 1'536, 3'072, false);
    plain.download_progress(
        0,
        1,
        1024U * 1024U,
        2U * 1024U * 1024U,
        false
    );
    plain.download_progress(
        0,
        1,
        1024ULL * 1024ULL * 1024ULL,
        2ULL * 1024ULL * 1024ULL * 1024ULL,
        false
    );
    if (plain_output.str() !=
        "Completed.\nSummary\n  Files: 12\n  Bytes: 42\n"
        "Microsoft Graph delta: 2 pages, 350 items scanned (continuing)\n"
        "Microsoft Graph delta: 3 pages, 412 items scanned (complete)\n"
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
    json.delta_progress(4, 625, false);
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
        false,
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
        true,
        {
            .bytes_per_second = 8,
            .elapsed_milliseconds = 1'500,
        }
    );
    std::istringstream json_lines{json_output.str()};
    std::string line;
    std::getline(json_lines, line);
    const auto message = nlohmann::json::parse(line);
    std::getline(json_lines, line);
    const auto section = nlohmann::json::parse(line);
    std::getline(json_lines, line);
    const auto delta_progress = nlohmann::json::parse(line);
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
        json_output.str().contains("\033[")) {
        return fail("JSON console output was invalid");
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
    quiet.delta_progress(1, 200, false);
    quiet.download_progress(0, 1, 1, 2, false);
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

    if (Console::parse_color_mode("auto") != ColorMode::automatic ||
        Console::parse_color_mode("always") != ColorMode::always ||
        Console::parse_color_mode("never") != ColorMode::never ||
        Console::parse_output_mode("text") != OutputMode::text ||
        Console::parse_output_mode("json") != OutputMode::json) {
        return fail("console option parsing returned the wrong mode");
    }
    try {
        static_cast<void>(Console::parse_color_mode("invalid"));
        return fail("invalid color mode was accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(Console::parse_output_mode("invalid"));
        return fail("invalid output mode was accepted");
    } catch (const std::invalid_argument&) {
    }
    return EXIT_SUCCESS;
}
