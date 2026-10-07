#include "onedrive/cli/console.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
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

constexpr unsigned completed_percentage = 100;
constexpr unsigned maximum_incomplete_percentage = 99;

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

std::string format_bytes(std::uint64_t bytes) {
    constexpr std::uint64_t unit_size = 1024;
    constexpr std::array<std::string_view, 3> units{
        "KiB",
        "MiB",
        "GiB",
    };
    if (bytes < unit_size) {
        return fmt::format("{} B", bytes);
    }

    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (true) {
        value /= static_cast<double>(unit_size);
        if (value < static_cast<double>(unit_size) ||
            unit + 1 == units.size()) {
            break;
        }
        ++unit;
    }
    return fmt::format("{:.1f} {}", value, units.at(unit));
}

std::string format_duration(std::uint64_t seconds) {
    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    const auto remaining_seconds = seconds % 60;
    return fmt::format(
        "{:02}:{:02}:{:02}",
        hours,
        minutes,
        remaining_seconds
    );
}

}  // namespace

Console::Console(
    ConsoleOptions options,
    std::ostream& output,
    std::ostream& error
)
    : options_{options},
      output_{&output},
      error_{&error},
      styled_{
        options_.output == OutputMode::text &&
        options_.color != ColorMode::never &&
        (options_.color == ColorMode::always ||
         (::isatty(STDOUT_FILENO) != 0 && std::getenv("NO_COLOR") == nullptr))
      },
      interactive_{
          options_.output == OutputMode::text && output_.get() == &std::cout &&
          ::isatty(STDOUT_FILENO) != 0
      } {}

void Console::message(
    MessageKind kind,
    std::string_view event,
    std::string_view text
) const {
    if (suppressed(options_, kind)) {
        return;
    }
    std::ostream& stream =
        kind == MessageKind::error ? *error_ : *output_;
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
        *output_ << nlohmann::json{
            {"event", event},
            {"values", std::move(values)},
        }.dump() << '\n';
        return;
    }

    if (styled_) {
        *output_ << "\033[1;36m" << title << "\033[0m\n";
    } else {
        *output_ << title << '\n';
    }
    std::size_t width = 0;
    for (const auto& field : fields) {
        width = std::max(width, field.label.size());
    }
    for (const auto& field : fields) {
        *output_ << fmt::format(
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
    util::ProgressState state
) const {
    if (options_.quiet) {
        return;
    }
    const bool completed = state == util::ProgressState::completed;
    if (options_.output == OutputMode::json) {
        *output_ << nlohmann::json{
            {"event", "delta_progress"},
            {"pages", pages},
            {"items", items},
            {"completed", completed},
        }.dump() << '\n';
        return;
    }
    if (!delta_progress_active_) {
        *output_ << "Microsoft Graph delta: ";
        delta_progress_active_ = true;
        delta_progress_pages_ = 0;
    }
    if (pages > delta_progress_pages_) {
        *output_ << std::string(pages - delta_progress_pages_, '.')
                 << std::flush;
        delta_progress_pages_ = pages;
    }
}

void Console::delta_summary(const DeltaSummary& summary) const {
    if (options_.quiet) {
        return;
    }
    if (options_.output == OutputMode::json) {
        *output_ << nlohmann::json{
            {"event", "delta_summary"},
            {"pages", summary.pages},
            {"scanned_items", summary.scanned_items},
            {"unique_changes", summary.unique_changes},
            {"files", summary.files},
            {"directories", summary.directories},
            {"deletions", summary.deletions},
        }.dump() << '\n';
        return;
    }
    const auto items_per_page =
        summary.pages == 0 ?
            0.0 :
            static_cast<double>(summary.scanned_items) /
                static_cast<double>(summary.pages);
    *output_ << (delta_progress_active_ ? "\n" : "")
             << fmt::format(
                    "Microsoft Graph delta complete: {} page{}, {} items "
                    "scanned, {} unique changes ({} files, {} folders, {} "
                    "deletions), {:.1f} items/page\n",
                    summary.pages,
                    summary.pages == 1 ? "" : "s",
                    summary.scanned_items,
                    summary.unique_changes,
                    summary.files,
                    summary.directories,
                    summary.deletions,
                    items_per_page
                );
    delta_progress_pages_ = 0;
    delta_progress_active_ = false;
}

void Console::blocked_item(
    std::string_view path,
    std::string_view reason_code,
    std::string_view reason_message
) const {
    if (options_.output == OutputMode::json) {
        *error_ << nlohmann::json{
            {"event", "item_blocked"},
            {"level", "warning"},
            {"path", path},
            {"reason_code", reason_code},
            {"message", reason_message},
        }.dump() << '\n';
        return;
    }
    *error_ << fmt::format(
        "Blocked '{}': {} ({})\n",
        path,
        reason_message,
        reason_code
    );
}

unsigned download_progress_percentage(
    std::size_t completed_files,
    std::size_t file_count,
    std::uint64_t downloaded,
    std::uint64_t total,
    util::ProgressState state
) noexcept {
    const bool completed = state == util::ProgressState::completed;
    auto percentage =
        total == 0 ?
            (file_count == 0 ?
                (completed ? completed_percentage : 0U) :
                static_cast<unsigned>(
                    std::min(
                        static_cast<double>(completed_percentage),
                        std::floor(
                            static_cast<double>(completed_files) * 100.0 /
                            static_cast<double>(file_count)
                        )
                    )
                )) :
            static_cast<unsigned>(std::min(
                static_cast<double>(completed_percentage),
                std::floor(
                    static_cast<double>(downloaded) * 100.0 /
                    static_cast<double>(total)
                )
            ));
    if (!completed) {
        percentage = std::min(
            percentage,
            maximum_incomplete_percentage
        );
    }
    return percentage;
}

void Console::download_progress(
    std::size_t completed_files,
    std::size_t file_count,
    std::uint64_t downloaded,
    std::uint64_t total,
    util::ProgressState state,
    const DownloadProgressMetrics& metrics
) const {
    if (options_.quiet) {
        return;
    }
    const bool completed = state == util::ProgressState::completed;
    const auto percentage = download_progress_percentage(
        completed_files,
        file_count,
        downloaded,
        total,
        state
    );
    if (options_.output == OutputMode::json) {
        *output_ << nlohmann::json{
            {"event", "download_progress"},
            {"completed_files", completed_files},
            {"file_count", file_count},
            {"downloaded_bytes", downloaded},
            {"total_bytes", total},
            {"percentage", percentage},
            {"completed", completed},
            {"bytes_per_second", metrics.bytes_per_second},
            {
                "estimated_seconds_remaining",
                metrics.estimated_seconds_remaining.has_value() ?
                    nlohmann::json(
                        *metrics.estimated_seconds_remaining
                    ) :
                    nlohmann::json(nullptr)
            },
            {
                "elapsed_milliseconds",
                metrics.elapsed_milliseconds
            },
        }.dump() << '\n';
        return;
    }

    auto line = fmt::format(
        "{}: {}/{} files, {}% ({}/{})",
        completed ? "Done" : "DL",
        completed_files,
        file_count,
        percentage,
        format_bytes(downloaded),
        format_bytes(total)
    );
    if (metrics.bytes_per_second != 0) {
        line += fmt::format(
            ", {}/s",
            format_bytes(metrics.bytes_per_second)
        );
    }
    if (completed) {
        line += fmt::format(
            ", elapsed {}",
            metrics.elapsed_milliseconds < 1000 ?
                "<1s" :
                format_duration(metrics.elapsed_milliseconds / 1000)
        );
    } else if (metrics.estimated_seconds_remaining.has_value()) {
        line += fmt::format(
            ", ETA {}",
            format_duration(*metrics.estimated_seconds_remaining)
        );
    }
    if (interactive_) {
        *output_ << '\r' << "\033[2K" << line;
        if (completed) {
            *output_ << '\n';
        }
        *output_ << std::flush;
        download_progress_active_ = !completed;
        return;
    }
    *output_ << line << '\n';
}

void Console::end_download_progress() const {
    if (interactive_ && download_progress_active_) {
        *output_ << '\n' << std::flush;
        download_progress_active_ = false;
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
    *output_ << prompt << std::flush;
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
