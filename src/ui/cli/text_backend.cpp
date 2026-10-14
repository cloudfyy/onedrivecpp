#include "ui/cli/backend_factory.hpp"
#include "ui/cli/format.hpp"
#include "ui/cli/message.hpp"

#include "onedrive/ui/cli/console.hpp"

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
        case MessageKind::trace:
            return {.symbol = "TR", .color = "\033[90m"};
        case MessageKind::debug:
            return {.symbol = "DB", .color = "\033[34m"};
        case MessageKind::information:
            return {.symbol = "->", .color = "\033[36m"};
        case MessageKind::warning:
            return {.symbol = "!!", .color = "\033[33m"};
        case MessageKind::error:
            return {.symbol = "XX", .color = "\033[31m"};
        case MessageKind::critical:
            return {.symbol = "XX", .color = "\033[31m"};
    }
    throw_unknown_message_kind();
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
        if (message_suppressed(options_, event.kind)) {
            return;
        }
        render(EndDownloadProgressEvent{});
        std::ostream& stream =
            event.kind == MessageKind::error ||
                    event.kind == MessageKind::critical
                ? error_ : output_;
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
        render(EndDownloadProgressEvent{});
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
            output_ << "Microsoft Graph delta: ";
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

    void render(const BlockedItemEvent& event) {
        error_ << fmt::format(
            "Blocked '{}': {} ({})\n",
            event.path,
            event.reason_message,
            event.reason_code
        );
    }

    void render(const DownloadProgressEvent& event) {
        render_transfer(event, event.downloaded, "DL", "Done");
    }

    void render(const UploadProgressEvent& event) {
        if (event.total == 0 && event.file_count == 0 &&
            event.state == util::ProgressState::ongoing) {
            if (!options_.quiet) {
                render(EndDownloadProgressEvent{});
                output_ << "UL: totals unknown; percentage unavailable\n";
            }
            return;
        }
        render_transfer(event, event.uploaded, "UL", "Upload done");
    }

    template <typename Progress>
    void render_transfer(
        const Progress& event,
        std::uint64_t bytes,
        std::string_view active_label,
        std::string_view completed_label
    ) {
        if (options_.quiet) {
            return;
        }
        const bool completed =
            event.state == util::ProgressState::completed;
        const auto percentage = download_progress_percentage(
            event.completed_files,
            event.file_count,
            bytes,
            event.total,
            event.state
        );
        auto line = fmt::format(
            "{}: {}/{} files, {}% ({}/{})",
            completed ? completed_label : active_label,
            event.completed_files,
            event.file_count,
            percentage,
            format_bytes(bytes),
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

    void render(const OperationStateEvent&) {}
    void render(const SyncPhaseEvent& event) {
        render(EndDownloadProgressEvent{});
        if (!options_.quiet) {
            output_ << "Phase: " << events::sync_phase_name(event.phase)
                    << '\n';
        }
    }
    void render(const SyncSummaryEvent&) {
    }
    void render(const events::SyncContextEvent& event) {
        if (!options_.quiet) {
            output_ << "Synchronization: "
                    << sync::sync_mode_name(event.direction)
                    << (event.preview ? " (preview)\n" : "\n");
        }
    }
    void render(const EndUploadProgressEvent&) {
        render(EndDownloadProgressEvent{});
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
