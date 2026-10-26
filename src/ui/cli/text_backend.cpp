#include "ui/cli/backend_factory.hpp"
#include "ui/cli/format.hpp"
#include "ui/cli/localization.hpp"
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
        case MessageKind::critical:
            return {.symbol = "XX", .color = "\033[31m"};
    }
    throw_unknown_message_kind();
}

class TextConsoleBackend final : public ConsoleBackend {
public:
    TextConsoleBackend(
        ConsoleOptions options, std::ostream& output, std::ostream& error
    )
        : options_{options},
          language_{resolve_language(options.language)},
          output_{output},
          error_{error},
          styled_{
              options_.color != ColorMode::never &&
              (options_.color == ColorMode::always ||
               (::isatty(STDOUT_FILENO) != 0 &&
                std::getenv("NO_COLOR") == nullptr))
          },
          interactive_{&output == &std::cout && ::isatty(STDOUT_FILENO) != 0},
          file_history_{options.file_history_limit} {
    }

    void finish() override {
        finish_file_history();
    }
    void finish_file_history() override {
        render_history_status();
    }

    void emit(const ConsoleEvent& event) override {
        std::visit(
            [this](const auto& value) { render(value); },
            event
        );
    }

    bool confirm(const ConfirmationRequest& request) override {
        output_ << translate_confirmation_prompt(request, language_)
                << std::flush;
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

    Language language() const noexcept override {
        return language_;
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
        const auto text = translate_message(event, language_);
        if (!styled_) {
            stream << text << '\n';
            return;
        }
        const auto style = style_for(event.kind);
        stream << style.color << "\033[1m" << style.symbol << "\033[0m "
               << text << '\n';
    }

    void render(const SectionEvent& event) {
        if (options_.quiet) {
            return;
        }
        render(EndDownloadProgressEvent{});
        const auto title =
            translate_section_title(event.event, event.title, language_);
        if (styled_) {
            output_ << "\033[1;36m" << title << "\033[0m\n";
        } else {
            output_ << title << '\n';
        }
        std::vector<std::pair<std::string, std::string>> rows;
        rows.reserve(event.fields.size());
        std::size_t width = 0;
        for (const auto& field : event.fields) {
            auto label =
                translate_field_label(field.key, field.label, language_);
            auto value =
                translate_field_value(field.key, field.value, language_);
            width = std::max(width, display_width(label));
            rows.emplace_back(std::move(label), std::move(value));
        }
        for (const auto& [label, value] : rows) {
            const auto label_width = display_width(label);
            const auto padding =
                width > label_width ? width - label_width : std::size_t{0};
            output_ << "  " << label << std::string(padding + 1, ' ') << value
                    << '\n';
        }
    }

    void render(const DeltaProgressEvent& event) {
        if (options_.quiet) {
            return;
        }
        if (!delta_progress_active_) {
            output_ << translate_text("Microsoft Graph delta: ", language_);
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
        output_ << (delta_progress_active_ ? "\n" : "")
                << format_delta_summary(event.summary, language_);
        delta_progress_pages_ = 0;
        delta_progress_active_ = false;
    }

    void render(const BlockedItemEvent& event) {
        error_ << format_blocked_item_line(event, language_);
    }

    void render(const DownloadProgressEvent& event) {
        render_transfer(event, event.downloaded, false);
    }

    void render(const UploadProgressEvent& event) {
        if (event.total == 0 && event.file_count == 0 &&
            event.state == util::ProgressState::ongoing) {
            if (!options_.quiet) {
                render(EndDownloadProgressEvent{});
                output_ << translate_text(
                    "UL: totals unknown; percentage unavailable\n", language_
                );
            }
            return;
        }
        render_transfer(event, event.uploaded, true);
    }

    template <typename Progress>
    void render_transfer(
        const Progress& event,
        std::uint64_t bytes,
        bool upload
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
        const auto formatted_bytes = format_bytes(bytes);
        const auto formatted_total = format_bytes(event.total);
        const auto formatted_rate =
            event.metrics.bytes_per_second != 0
                ? std::optional<std::string>{format_bytes(
                      event.metrics.bytes_per_second
                  )}
                : std::nullopt;
        std::optional<std::string> duration;
        if (completed) {
            duration = event.metrics.elapsed_milliseconds < 1000
                           ? "<1s"
                           : format_duration(
                                 event.metrics.elapsed_milliseconds / 1000
                             );
        } else if (event.metrics.estimated_seconds_remaining.has_value()) {
            duration =
                format_duration(*event.metrics.estimated_seconds_remaining);
        }
        const auto line = format_text_transfer_progress(
            upload,
            completed,
            event.completed_files,
            event.file_count,
            percentage,
            formatted_bytes,
            formatted_total,
            formatted_rate ? std::optional<std::string_view>{*formatted_rate}
                           : std::nullopt,
            duration ? std::optional<std::string_view>{*duration}
                     : std::nullopt,
            language_
        );
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
            output_ << format_text_sync_phase_line(event.phase, language_);
        }
    }
    void render(const SyncSummaryEvent&) {
        render_history_status();
    }
    void render(const events::FileOperationEvent& event) {
        file_history_.add(event);
        history_changed_ = true;
        if (options_.quiet) {
            return;
        }
        render(EndDownloadProgressEvent{});
        if (!history_heading_printed_) {
            output_ << translate_text(
                "Completed file operations (this process):\n"
                "Path  Action / details  Time (UTC)\n",
                language_
            );
            history_heading_printed_ = true;
        }
        output_ << format_file_operation(event, language_) << '\n'
                << std::flush;
    }
    void render_history_status() {
        if (history_changed_ && !options_.quiet) {
            render(EndDownloadProgressEvent{});
            output_ << format_file_history_status(file_history_, language_)
                    << '\n';
        }
        history_changed_ = false;
    }
    void render(const events::SyncContextEvent& event) {
        if (!options_.quiet) {
            output_ << format_text_sync_context_line(
                event.direction, event.preview, language_
            );
        }
    }
    void render(const EndUploadProgressEvent&) {
        render(EndDownloadProgressEvent{});
    }

    ConsoleOptions options_;
    Language language_{Language::english};
    std::ostream& output_;
    std::ostream& error_;
    bool styled_{false};
    bool interactive_{false};
    std::size_t delta_progress_pages_{0};
    bool delta_progress_active_{false};
    bool download_progress_active_{false};
    events::FileHistory file_history_;
    bool history_heading_printed_{false};
    bool history_changed_{false};
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
