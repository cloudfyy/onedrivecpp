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
            output_ << (is_chinese(language_) ? "Microsoft Graph 增量同步："
                                              : "Microsoft Graph delta: ");
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
        render_transfer(
            event,
            event.downloaded,
            is_chinese(language_) ? "下载" : "DL",
            is_chinese(language_) ? "完成" : "Done"
        );
    }

    void render(const UploadProgressEvent& event) {
        if (event.total == 0 && event.file_count == 0 &&
            event.state == util::ProgressState::ongoing) {
            if (!options_.quiet) {
                render(EndDownloadProgressEvent{});
                output_ << (is_chinese(language_)
                                ? "上传：总量未知；无法计算百分比\n"
                                : "UL: totals unknown; percentage unavailable\n");
            }
            return;
        }
        render_transfer(
            event,
            event.uploaded,
            is_chinese(language_) ? "上传" : "UL",
            is_chinese(language_) ? "上传完成" : "Upload done"
        );
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
        const bool chinese = is_chinese(language_);
        auto line =
            chinese
                ? fmt::format(
                      "{}：{}/{} 个文件，{}%（{}/{}）",
                      completed ? completed_label : active_label,
                      event.completed_files,
                      event.file_count,
                      percentage,
                      format_bytes(bytes),
                      format_bytes(event.total)
                  )
                : fmt::format(
                      "{}: {}/{} files, {}% ({}/{})",
                      completed ? completed_label : active_label,
                      event.completed_files,
                      event.file_count,
                      percentage,
                      format_bytes(bytes),
                      format_bytes(event.total)
                  );
        if (event.metrics.bytes_per_second != 0) {
            line += chinese
                ? fmt::format(
                      "，{}/s", format_bytes(event.metrics.bytes_per_second)
                  )
                : fmt::format(
                      ", {}/s", format_bytes(event.metrics.bytes_per_second)
                  );
        }
        if (completed) {
            const auto elapsed =
                event.metrics.elapsed_milliseconds < 1000
                    ? "<1s"
                    : format_duration(
                          event.metrics.elapsed_milliseconds / 1000
                      );
            line += chinese ? fmt::format("，用时 {}", elapsed)
                            : fmt::format(", elapsed {}", elapsed);
        } else if (
            event.metrics.estimated_seconds_remaining.has_value()
        ) {
            const auto eta = format_duration(
                *event.metrics.estimated_seconds_remaining
            );
            line += chinese ? fmt::format("，预计剩余 {}", eta)
                            : fmt::format(", ETA {}", eta);
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
            output_ << (is_chinese(language_) ? "阶段：" : "Phase: ")
                    << translate_sync_phase(event.phase, language_) << '\n';
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
            output_ << (is_chinese(language_)
                            ? "已完成的文件操作（当前进程）：\n"
                              "路径  操作 / 详情  时间 (UTC)\n"
                            : "Completed file operations (this process):\n"
                              "Path  Action / details  Time (UTC)\n");
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
            if (is_chinese(language_)) {
                output_ << "同步："
                        << translate_sync_mode(event.direction, language_)
                        << (event.preview ? "（预览）\n" : "\n");
            } else {
                output_ << "Synchronization: "
                        << sync::sync_mode_name(event.direction)
                        << (event.preview ? " (preview)\n" : "\n");
            }
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
