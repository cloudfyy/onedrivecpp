#include "onedrive/cli/console.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

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
    plain.download_progress("file.bin", 1, 2, 5, 10, false);
    plain.download_progress("file.bin", 1, 2, 10, 10, true);
    if (plain_output.str() !=
        "Completed.\nSummary\n  Files: 12\n  Bytes: 42\n"
        "Microsoft Graph delta: 2 pages, 350 items scanned (continuing)\n"
        "Microsoft Graph delta: 3 pages, 412 items scanned (complete)\n"
        "[1/2] Downloading 'file.bin': 50% (5/10 bytes)\n"
        "[1/2] Downloading 'file.bin': 100% (10/10 bytes)\n" ||
        !plain_error.str().empty()) {
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
    json.download_progress("file.bin", 1, 1, 4, 8, false);
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
    if (message.at("event") != "phase" ||
        message.at("message") != "Working" ||
        section.at("event") != "summary" ||
        section.at("values").at("files") != "12" ||
        delta_progress.at("event") != "delta_progress" ||
        delta_progress.at("pages") != 4 ||
        delta_progress.at("items") != 625 ||
        delta_progress.at("completed") != false ||
        download_progress.at("event") != "download_progress" ||
        download_progress.at("path") != "file.bin" ||
        download_progress.at("downloaded_bytes") != 4 ||
        download_progress.at("total_bytes") != 8 ||
        download_progress.at("percentage") != 50 ||
        download_progress.at("completed") != false ||
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
    quiet.download_progress("hidden.bin", 1, 1, 1, 2, false);
    quiet.message(MessageKind::warning, "warning", "Visible warning");
    quiet.message(MessageKind::error, "error", "Visible error");
    if (quiet_output.str() != "Visible warning\n" ||
        quiet_error.str() != "Visible error\n") {
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
