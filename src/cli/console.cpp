#include "onedrive/cli/console.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace onedrive::cli {
namespace {

struct Style {
    std::string_view symbol;
    std::string_view color;
};

Style style_for(MessageKind kind) {
    switch (kind) {
        case MessageKind::information:
            return {.symbol = "->", .color = "\033[36m"};
        case MessageKind::success:
            return {.symbol = "OK", .color = "\033[32m"};
        case MessageKind::warning:
            return {.symbol = "!!", .color = "\033[33m"};
        case MessageKind::error:
            return {.symbol = "XX", .color = "\033[31m"};
    }
    throw std::logic_error{"unknown console message kind"};
}

std::string_view level_for(MessageKind kind) {
    switch (kind) {
        case MessageKind::information:
            return "info";
        case MessageKind::success:
            return "success";
        case MessageKind::warning:
            return "warning";
        case MessageKind::error:
            return "error";
    }
    throw std::logic_error{"unknown console message kind"};
}

bool suppressed(const ConsoleOptions& options, MessageKind kind) {
    return options.quiet &&
           kind != MessageKind::warning &&
           kind != MessageKind::error;
}

}  // namespace

Console::Console(
    ConsoleOptions options,
    std::ostream& output,
    std::ostream& error
)
    : options_{options}, output_{output}, error_{error} {
    styled_ =
        options_.output == OutputMode::text &&
        options_.color != ColorMode::never &&
        (options_.color == ColorMode::always ||
         (::isatty(STDOUT_FILENO) != 0 && std::getenv("NO_COLOR") == nullptr));
    interactive_ =
        options_.output == OutputMode::text && &output_ == &std::cout &&
        ::isatty(STDOUT_FILENO) != 0;
}

void Console::message(
    MessageKind kind,
    std::string_view event,
    std::string_view text
) const {
    if (suppressed(options_, kind)) {
        return;
    }
    std::ostream& stream =
        kind == MessageKind::error ? error_ : output_;
    if (options_.output == OutputMode::json) {
        stream << nlohmann::json{
            {"event", event},
            {"level", level_for(kind)},
            {"message", text},
        }.dump() << '\n';
        return;
    }
    if (!styled_) {
        stream << text << '\n';
        return;
    }
    const auto style = style_for(kind);
    stream << style.color << "\033[1m" << style.symbol << "\033[0m "
           << text << '\n';
}

void Console::section(
    std::string_view event,
    std::string_view title,
    const std::vector<Field>& fields
) const {
    if (options_.quiet) {
        return;
    }
    if (options_.output == OutputMode::json) {
        nlohmann::json values = nlohmann::json::object();
        for (const auto& field : fields) {
            values[field.key] = field.value;
        }
        output_ << nlohmann::json{
            {"event", event},
            {"values", std::move(values)},
        }.dump() << '\n';
        return;
    }

    if (styled_) {
        output_ << "\033[1;36m" << title << "\033[0m\n";
    } else {
        output_ << title << '\n';
    }
    std::size_t width = 0;
    for (const auto& field : fields) {
        width = std::max(width, field.label.size());
    }
    for (const auto& field : fields) {
        output_ << fmt::format(
            "  {:<{}} {}\n",
            field.label,
            width,
            field.value
        );
    }
}

void Console::delta_progress(
    std::size_t pages,
    std::size_t items,
    bool completed
) const {
    if (options_.quiet) {
        return;
    }
    if (options_.output == OutputMode::json) {
        output_ << nlohmann::json{
            {"event", "delta_progress"},
            {"pages", pages},
            {"items", items},
            {"completed", completed},
        }.dump() << '\n';
        return;
    }
    output_ << fmt::format(
        "Microsoft Graph delta: {} page{}, {} item{} scanned ({})\n",
        pages,
        pages == 1 ? "" : "s",
        items,
        items == 1 ? "" : "s",
        completed ? "complete" : "continuing"
    );
}

void Console::blocked_item(
    std::string_view path,
    std::string_view reason_code,
    std::string_view reason_message
) const {
    if (options_.output == OutputMode::json) {
        error_ << nlohmann::json{
            {"event", "item_blocked"},
            {"level", "warning"},
            {"path", path},
            {"reason_code", reason_code},
            {"message", reason_message},
        }.dump() << '\n';
        return;
    }
    error_ << fmt::format(
        "Blocked '{}': {} ({})\n",
        path,
        reason_message,
        reason_code
    );
}

void Console::download_progress(
    std::string_view path,
    std::size_t file_index,
    std::size_t file_count,
    std::uint64_t downloaded,
    std::uint64_t total,
    bool completed
) const {
    if (options_.quiet) {
        return;
    }
    const auto percentage =
        total == 0 ?
            (completed ? 100U : 0U) :
            static_cast<unsigned>(std::min(
                100.0,
                std::floor(
                    static_cast<double>(downloaded) * 100.0 /
                    static_cast<double>(total)
                )
            ));
    if (options_.output == OutputMode::json) {
        output_ << nlohmann::json{
            {"event", "download_progress"},
            {"path", path},
            {"file_index", file_index},
            {"file_count", file_count},
            {"downloaded_bytes", downloaded},
            {"total_bytes", total},
            {"percentage", percentage},
            {"completed", completed},
        }.dump() << '\n';
        return;
    }

    const auto line = fmt::format(
        "[{}/{}] Downloading '{}': {}%",
        file_index,
        file_count,
        path,
        percentage
    );
    if (interactive_) {
        output_ << '\r' << "\033[2K" << line;
        if (completed) {
            output_ << '\n';
        }
        output_ << std::flush;
        return;
    }
    output_ << line << '\n';
}

void Console::end_download_progress() const {
    if (interactive_) {
        output_ << '\n' << std::flush;
    }
}

bool Console::confirm(
    std::string_view event,
    std::string_view prompt,
    std::string_view expected
) const {
    if (options_.output == OutputMode::json) {
        throw std::runtime_error(
            "interactive confirmation is unavailable with --output=json; "
            "use --yes to confirm explicitly"
        );
    }
    output_ << prompt << std::flush;
    std::string confirmation;
    const bool matched =
        static_cast<bool>(std::getline(std::cin, confirmation)) &&
        confirmation == expected;
    if (!matched && !options_.quiet) {
        message(
            MessageKind::warning,
            event,
            "Confirmation did not match."
        );
    }
    return matched;
}

OutputMode Console::output_mode() const noexcept {
    return options_.output;
}

ColorMode Console::parse_color_mode(std::string_view value) {
    if (value == "auto") {
        return ColorMode::automatic;
    }
    if (value == "always") {
        return ColorMode::always;
    }
    if (value == "never") {
        return ColorMode::never;
    }
    throw std::invalid_argument{"invalid color mode"};
}

OutputMode Console::parse_output_mode(std::string_view value) {
    if (value == "text") {
        return OutputMode::text;
    }
    if (value == "json") {
        return OutputMode::json;
    }
    throw std::invalid_argument{"invalid output mode"};
}

std::ostream& Console::default_output() {
    return std::cout;
}

std::ostream& Console::default_error() {
    return std::cerr;
}

}  // namespace onedrive::cli
