#include "cli/backend_factory.hpp"
#include "cli/format.hpp"

#include "onedrive/cli/console.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

namespace onedrive::cli::detail {
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

bool suppressed(const ConsoleOptions& options, MessageKind kind) {
    return options.quiet &&
           kind != MessageKind::warning &&
           kind != MessageKind::error;
}

class TextConsoleBackend final : public ConsoleBackend {
public:
    TextConsoleBackend(
        ConsoleOptions options,
        std::ostream& output,
        std::ostream& error
    )
        : options_{options},
          output_{output},
          error_{error},
          styled_{
              options_.color != ColorMode::never &&
              (options_.color == ColorMode::always ||
               (::isatty(STDOUT_FILENO) != 0 &&
                std::getenv("NO_COLOR") == nullptr))
          },
          interactive_{
              &output == &std::cout &&
              ::isatty(STDOUT_FILENO) != 0
          } {}

    void emit(const ConsoleEvent& event) override {
        std::visit(
            [this](const auto& value) { render(value); },
            event
        );
    }

    bool confirm(const ConfirmationRequest& request) override {
        output_ << request.prompt << std::flush;
        std::string confirmation;
        return static_cast<bool>(std::getline(std::cin, confirmation)) &&
               confirmation == request.expected;
    }

    OutputMode output_mode() const noexcept override {
        return OutputMode::text;
    }

    UiMode ui_mode() const noexcept override {
        return UiMode::console;
    }

private:
    void render(const MessageEvent& event) {
        if (suppressed(options_, event.kind)) {
            return;
        }
        std::ostream& stream =
            event.kind == MessageKind::error ? error_ : output_;
        if (!styled_) {
            stream << event.text << '\n';
            return;
        }
        const auto style = style_for(event.kind);
        stream << style.color << "\033[1m" << style.symbol << "\033[0m "
               << event.text << '\n';
    }

    void render(const SectionEvent& event) {
        if (options_.quiet) {
            return;
        }
        if (styled_) {
            output_ << "\033[1;36m" << event.title << "\033[0m\n";
        } else {
            output_ << event.title << '\n';
        }
        std::size_t width = 0;
        for (const auto& field : event.fields) {
            width = std::max(width, field.label.size());
        }
        for (const auto& field : event.fields) {
            output_ << fmt::format(
                "  {:<{}} {}\n",
                field.label,
                width,
                field.value
            );
        }
    }

    void render(const DeltaProgressEvent& event) {
        if (options_.quiet) {
            return;
        }
        if (!delta_progress_active_) {
            output_ << "Cloud check: ";
            delta_progress_active_ = true;
            delta_progress_pages_ = 0;
        }
        if (event.pages > delta_progress_pages_) {
            output_ << std::string(
                event.pages - delta_progress_pages_, '.'
            ) << std::flush;
            delta_progress_pages_ = event.pages;
        }
    }

    void render(const DeltaSummaryEvent& event) {
        if (options_.quiet) {
            return;
        }
        const auto& summary = event.summary;
        const auto items_per_page =
            summary.pages == 0 ?
                0.0 :
                static_cast<double>(summary.scanned_items) /
                    static_cast<double>(summary.pages);
        output_ << (delta_progress_active_ ? "\n" : "")
                << fmt::format(
                       "Cloud check complete: {} batch{}, {} items checked, "
                       "{} changes ({} files, {} folders, {} deletions), "
                       "{:.1f} items/batch\n",
                       summary.pages,
                       summary.pages == 1 ? "" : "es",
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

    void render(const BlockedItemEvent& event) {
        error_ << fmt::format(
            "Blocked '{}': {} ({})\n",
            event.path,
            event.reason_message,
            event.reason_code
        );
    }

    void render(const DownloadProgressEvent& event) {
        if (options_.quiet) {
            return;
        }
        const bool completed =
            event.state == util::ProgressState::completed;
        const auto percentage = download_progress_percentage(
            event.completed_files,
            event.file_count,
            event.downloaded,
            event.total,
            event.state
        );
        auto line = fmt::format(
            "{}: {}/{} files, {}% ({}/{})",
            completed ? "Done" : "DL",
            event.completed_files,
            event.file_count,
            percentage,
            format_bytes(event.downloaded),
            format_bytes(event.total)
        );
        if (event.metrics.bytes_per_second != 0) {
            line += fmt::format(
                ", {}/s",
                format_bytes(event.metrics.bytes_per_second)
            );
        }
        if (completed) {
            line += fmt::format(
                ", elapsed {}",
                event.metrics.elapsed_milliseconds < 1000 ?
                    "<1s" :
                    format_duration(
                        event.metrics.elapsed_milliseconds / 1000
                    )
            );
        } else if (
            event.metrics.estimated_seconds_remaining.has_value()
        ) {
            line += fmt::format(
                ", ETA {}",
                format_duration(
                    *event.metrics.estimated_seconds_remaining
                )
            );
        }
        if (interactive_) {
            output_ << '\r' << "\033[2K" << line;
            if (completed) {
                output_ << '\n';
            }
            output_ << std::flush;
            download_progress_active_ = !completed;
            return;
        }
        output_ << line << '\n';
    }

    void render(const EndDownloadProgressEvent&) {
        if (interactive_ && download_progress_active_) {
            output_ << '\n' << std::flush;
            download_progress_active_ = false;
        }
    }

    ConsoleOptions options_;
    std::ostream& output_;
    std::ostream& error_;
    bool styled_{false};
    bool interactive_{false};
    std::size_t delta_progress_pages_{0};
    bool delta_progress_active_{false};
    bool download_progress_active_{false};
};

}  // namespace

std::unique_ptr<ConsoleBackend> make_text_console_backend(
    ConsoleOptions options,
    std::ostream& output,
    std::ostream& error
) {
    return std::make_unique<TextConsoleBackend>(
        options, output, error
    );
}

}  // namespace onedrive::cli::detail
