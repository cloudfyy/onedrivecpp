#include "ui/cli/backend_factory.hpp"
#include "ui/cli/format.hpp"
#include "ui/cli/localization.hpp"
#include "ui/cli/message.hpp"

#include "onedrive/ui/cli/console.hpp"
#include "onedrive/version.hpp"

#include <fmt/format.h>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

namespace onedrive::cli::detail {
namespace {

using namespace ftxui;

constexpr std::size_t maximum_messages = 6;

struct ThemePalette {
    Color primary;
    Color accent;
    Color success;
    Color warning;
    Color error;
};

ThemePalette palette_for(TuiTheme theme) {
    switch (theme) {
        case TuiTheme::hacker:
            return {
                .primary = Color::GreenLight,
                .accent = Color::Green,
                .success = Color::GreenLight,
                .warning = Color::YellowLight,
                .error = Color::RedLight,
            };
        case TuiTheme::ocean:
            return {
                .primary = Color::CyanLight,
                .accent = Color::BlueLight,
                .success = Color::GreenLight,
                .warning = Color::YellowLight,
                .error = Color::RedLight,
            };
        case TuiTheme::amber:
            return {
                .primary = Color::YellowLight,
                .accent = Color::Yellow,
                .success = Color::GreenLight,
                .warning = Color::YellowLight,
                .error = Color::RedLight,
            };
        case TuiTheme::synthwave:
            return {
                .primary = Color::MagentaLight,
                .accent = Color::CyanLight,
                .success = Color::GreenLight,
                .warning = Color::YellowLight,
                .error = Color::RedLight,
            };
    }
    return palette_for(TuiTheme::hacker);
}

const std::string&
field_value(const SectionEvent& section, std::string_view key) {
    const auto field = std::ranges::find(section.fields, key, &Field::key);
    if (field == section.fields.end()) {
        throw std::runtime_error(
            fmt::format("{} result is missing field '{}'", section.event, key)
        );
    }
    return field->value;
}

class FtxuiConsoleBackend final : public ConsoleBackend {
public:
    FtxuiConsoleBackend(
        ConsoleOptions options,
        std::ostream& output,
        std::ostream&,
        std::size_t columns,
        std::size_t rows
    )
        : output_{output},
          columns_{columns},
          rows_{rows},
          language_{resolve_language(options.language)},
          theme_{options.theme},
          view_{options.view},
          palette_{palette_for(theme_)},
          styled_{
              options.color != ColorMode::never &&
              (options.color == ColorMode::always ||
               std::getenv("NO_COLOR") == nullptr)
          },
          file_history_{options.file_history_limit} {
        enter_fullscreen();
        try {
            render();
        } catch (...) {
            leave_fullscreen();
            throw;
        }
    }

    ~FtxuiConsoleBackend() override {
        leave_fullscreen();
    }
    FtxuiConsoleBackend(const FtxuiConsoleBackend&) = delete;
    FtxuiConsoleBackend& operator=(const FtxuiConsoleBackend&) = delete;
    FtxuiConsoleBackend(FtxuiConsoleBackend&&) = delete;
    FtxuiConsoleBackend& operator=(FtxuiConsoleBackend&&) = delete;

    void finish() override {
        if (view_ == TuiView::sync || view_ == TuiView::watch ||
            view_ == TuiView::download) {
            finish_file_history();
            return;
        }
        switch (view_) {
            case TuiView::health:
            case TuiView::status:
            case TuiView::blocked:
            case TuiView::drives:
            case TuiView::shared:
            case TuiView::sites:
            case TuiView::quota:
            case TuiView::storage:
            case TuiView::partials:
            case TuiView::files:
            case TuiView::verify:
            case TuiView::config:
                break;
            default:
                return;
        }
        if (finished_) {
            return;
        }
        finished_ = true;
        result_index_ = 0;
        if (&output_ == &std::cout && ::isatty(STDIN_FILENO) != 0 &&
            ::isatty(STDOUT_FILENO) != 0) {
            browse_results();
            return;
        }
        render();
        std::string input;
        while (std::getline(std::cin, input)) {
            if (handle_result_key(
                    input.empty() ? Event::Return : Event::Special(input)
                )) {
                break;
            }
            render();
        }
        if (std::cin.bad()) {
            throw std::runtime_error("cannot read terminal completion input");
        }
    }

    void emit(const ConsoleEvent& event) override {
        std::visit(
            [this](const auto& value) { update(value); },
            event
        );
        render();
    }

    void finish_file_history() override {
        if (history_printed_ || file_history_.entries().empty()) {
            return;
        }
        leave_fullscreen();
        output_ << translate_text(
            "\nCompleted file operations (this process):\n"
            "Path  Action / details  Time (UTC)\n",
            language_
        );
        for (const auto& operation : file_history_.entries()) {
            output_ << format_file_operation(operation, language_) << '\n';
        }
        output_ << format_file_history_status(file_history_, language_) << '\n'
                << std::flush;
        history_printed_ = true;
    }

    bool confirm(const ConfirmationRequest& request) override {
        output_ << "\033[2J\033[H\033[?25h"
                << translate_confirmation_prompt(request, language_)
                << std::flush;
        std::string confirmation;
        const bool matched =
            static_cast<bool>(std::getline(std::cin, confirmation)) &&
            confirmation == request.expected;
        render();
        return matched;
    }

    OutputMode output_mode() const noexcept override {
        return OutputMode::text;
    }

    UiMode ui_mode() const noexcept override {
        return UiMode::tui;
    }

    Language language() const noexcept override {
        return language_;
    }

private:
    struct DashboardMessage {
        MessageKind kind;
        std::string text;
    };

    bool handle_result_key(const Event& event) {
        if (event == Event::Return || event == Event::Character('q') ||
            event == Event::Character('Q')) {
            return true;
        }
        const bool partials = view_ == TuiView::partials;
        if ((event == (partials ? Event::ArrowDown : Event::ArrowRight) ||
             event == Event::Character(partials ? 'j' : 'n')) &&
            result_index_ + 1 < result_pages_.size()) {
            ++result_index_;
        } else if ((event == (partials ? Event::ArrowUp : Event::ArrowLeft) ||
                    event == Event::Character(partials ? 'k' : 'p')) &&
                   result_index_ > 0) {
            --result_index_;
        }
        return false;
    }

    void browse_results() {
        auto screen = ScreenInteractive::Fullscreen();
        screen.TrackMouse(false);
        auto component = CatchEvent(
            Renderer([this] { return dashboard(Terminal::Size().dimy); }),
            [&](const Event& event) {
                if (handle_result_key(event)) {
                    screen.Exit();
                }
                return true;
            }
        );
        leave_fullscreen();
        screen.Loop(std::move(component));
    }

    Element with_color(Element element, Color value) const {
        return styled_ ?
            std::move(element) | color(value) :
            std::move(element);
    }

    Element emphasized(Element element) const {
        return styled_ ?
            std::move(element) | bold :
            std::move(element);
    }

    Color color_for(MessageKind kind) const {
        switch (kind) {
            case MessageKind::trace:
            case MessageKind::debug:
            case MessageKind::information:
                return palette_.accent;
            case MessageKind::warning:
                return palette_.warning;
            case MessageKind::error:
            case MessageKind::critical:
                return palette_.error;
        }
        throw_unknown_message_kind();
    }

    void update(const MessageEvent& event) {
        auto message = translate_tui_message(event, language_);
        if (!message) {
            return;
        }
        messages_.push_back({event.kind, std::move(*message)});
        if (messages_.size() > maximum_messages) {
            messages_.erase(messages_.begin());
        }
    }

    void update(const SectionEvent& event) {
        section_ = event;
        section_.title =
            translate_tui_section_title(event.event, event.title, language_);
        for (auto& field : section_.fields) {
            field.label =
                translate_tui_field_label(field.key, field.label, language_);
            field.value =
                translate_field_value(field.key, field.value, language_);
        }
        if ((view_ == TuiView::drives && event.event == "drive") ||
            (view_ == TuiView::blocked && event.event == "blocked_item") ||
            (view_ == TuiView::partials && event.event == "partial_download") ||
            (view_ == TuiView::files && event.event == "downloaded_file") ||
            (view_ == TuiView::verify && event.event == "file_verification") ||
            ((view_ == TuiView::health || view_ == TuiView::status ||
              view_ == TuiView::shared || view_ == TuiView::sites ||
              view_ == TuiView::quota || view_ == TuiView::storage ||
              view_ == TuiView::config) &&
             !section_.fields.empty())) {
            result_pages_.push_back(section_);
            result_index_ = result_pages_.size() - 1;
        }
        if (view_ == TuiView::partials &&
            event.event == "partial_download_summary") {
            partial_summary_ = section_;
        }
    }

    void update(const DeltaProgressEvent& event) {
        delta_items_ = event.items;
        delta_complete_ =
            event.state == util::ProgressState::completed;
    }

    void update(const DeltaSummaryEvent& event) {
        delta_items_ = event.summary.scanned_items;
        delta_changes_ = event.summary.unique_changes;
        delta_complete_ = true;
    }

    void update(const BlockedItemEvent& event) {
        ++blocked_items_;
        last_blocked_ = format_tui_blocked_item(event, language_);
    }

    void update(const DownloadProgressEvent& event) {
        download_ = event;
    }

    void update(const EndDownloadProgressEvent&) {}

    void update(const UploadProgressEvent& event) {
        upload_ = event;
    }
    void update(const EndUploadProgressEvent&) {
    }
    void update(const SyncPhaseEvent& event) {
        phase_ = event.phase;
        if (event.phase == events::SyncPhase::preparing) {
            round_incomplete_ = true;
            download_.reset();
            upload_.reset();
        }
    }
    void update(const events::SyncContextEvent& event) {
        context_ = event;
    }
    void update(const SyncSummaryEvent& event) {
        summary_ = event;
        round_incomplete_ = event.blocked != 0;
        if (monitoring_) {
            phase_ = events::SyncPhase::waiting;
        }
    }
    void update(const events::FileOperationEvent& event) {
        file_history_.add(event);
    }
    void update(const OperationStateEvent& event) {
        if (event.operation == OperationKind::monitoring) {
            monitoring_ = event.state == OperationState::watching;
            if (monitoring_) {
                phase_ = events::SyncPhase::waiting;
            }
        }
    }

    Element header() const {
        const auto view_name = translate_tui_view_title(view_, language_);
        return hbox({
            with_color(
                emphasized(text(fmt::format(
                    " ONEDRIVE // {}  v{} ",
                    view_name,
                    build_info::version
                ))),
                palette_.primary
            ),
            filler(),
        });
    }

    Element cloud_status() const {
        auto status = format_tui_cloud_status(
            delta_items_,
            delta_complete_ ? std::optional<std::size_t>{delta_changes_}
                            : std::nullopt,
            language_
        );
        return with_color(
            text(std::move(status)),
            delta_complete_ ? palette_.success : palette_.accent
        );
    }

    template <typename Progress>
    Element transfer_status(
        const Progress& transfer,
        std::uint64_t bytes,
        std::string_view title,
        bool compact = false
    ) const {
        if (transfer.total == 0 && transfer.file_count == 0 &&
            transfer.state == util::ProgressState::ongoing) {
            if (compact) {
                return text(
                    format_tui_transfer_unknown_compact(title, language_)
                );
            }
            return vbox({
                       emphasized(text(std::string{title})),
                       text(std::string{translate_text(
                           "Totals unknown; percentage unavailable.", language_
                       )}),
                   }) |
                   border;
        }
        const auto percentage = download_progress_percentage(
            transfer.completed_files,
            transfer.file_count,
            bytes,
            transfer.total,
            transfer.state
        );
        auto progress = gauge(
            static_cast<float>(percentage) / 100.0F
        );
        progress = with_color(std::move(progress), palette_.primary);
        const bool completed =
            transfer.state == util::ProgressState::completed;
        std::optional<std::string> duration;
        if (completed) {
            duration = transfer.metrics.elapsed_milliseconds < 1000
                           ? "<1s"
                           : format_duration(
                                 transfer.metrics.elapsed_milliseconds / 1000
                             );
        } else if (transfer.metrics.estimated_seconds_remaining.has_value()) {
            duration = format_duration(
                *transfer.metrics.estimated_seconds_remaining
            );
        }
        const auto transferred_str = format_bytes(bytes);
        const auto total_str = format_bytes(transfer.total);
        const auto rate_str =
            format_bytes(transfer.metrics.bytes_per_second);
        auto details = format_tui_transfer_details(
            percentage,
            transferred_str,
            total_str,
            rate_str,
            completed,
            duration ? std::optional<std::string_view>{*duration}
                     : std::nullopt,
            language_
        );
        const auto heading = format_tui_transfer_heading(
            title, transfer.completed_files, transfer.file_count, language_
        );
        if (compact) {
            return with_color(
                text(heading + " // " + details), palette_.primary
            );
        }
        return vbox({
                   emphasized(text(heading)),
                   hbox({
                       text("["),
                       std::move(progress) | flex,
                       text("]"),
                   }),
                   text(std::move(details)),
               }) |
               border;
    }

    Element partial_results(int rows_available) const {
        Elements content;
        if (partial_summary_) {
            Elements summary;
            for (const auto& field : partial_summary_->fields) {
                summary.push_back(text(field.label + " " + field.value + "  "));
            }
            content.push_back(flexbox(std::move(summary)));
        }
        content.push_back(emphasized(text(format_tui_partial_heading(
            result_index_ + 1, result_pages_.size(), language_
        ))));
        content.push_back(hbox({
            text(std::string{translate_text("  PATH", language_)}) | xflex,
            text(std::string{translate_text(
                "STATUS  SAVED PROGRESS / TOTAL", language_
            )}),
        }));
        Elements rows;
        for (std::size_t index = 0; index < result_pages_.size(); ++index) {
            const auto& item = result_pages_[index];
            auto row = hbox({
                text(index == result_index_ ? "> " : "  "),
                text(field_value(item, "remote_path")) | xflex,
                text(
                    fmt::format(
                        " {}  {} / {}",
                        field_value(item, "status"),
                        field_value(item, "completed_bytes"),
                        field_value(item, "expected_bytes")
                    )
                ),
            });
            if (index == result_index_) {
                row = with_color(std::move(row), palette_.primary) | focus;
            }
            rows.push_back(std::move(row));
        }
        content.push_back(
            vbox(std::move(rows)) | vscroll_indicator | yframe |
            size(HEIGHT, GREATER_THAN, 1) | yflex
        );
        if (rows_available >= 18) {
            Elements details;
            for (const auto& field : result_pages_[result_index_].fields) {
                if (field.key == "remote_path" || field.key == "destination" ||
                    field.key == "temporary_path" ||
                    field.key == "actual_bytes") {
                    details.push_back(paragraph(field.label + " " + field.value)
                    );
                }
            }
            content.push_back(
                vbox(std::move(details)) |
                size(HEIGHT, LESS_THAN, rows_available / 3)
            );
        }
        return vbox(std::move(content));
    }

    Element dashboard(int rows_available) const {
        const bool live = view_ == TuiView::sync || view_ == TuiView::watch ||
                          view_ == TuiView::download;
        const bool waiting = live && phase_ == events::SyncPhase::waiting;
        const bool show_download =
            download_.has_value() && !waiting &&
            !(live && monitoring_ &&
              download_->state == util::ProgressState::completed);
        const bool show_upload =
            upload_.has_value() && !waiting &&
            !(live && monitoring_ &&
              upload_->state == util::ProgressState::completed);
        // Reserve status, controls and a small activity area before panels.
        int spare_rows =
            rows_available - 4 - phase_.has_value() - (blocked_items_ != 0) -
            (view_ == TuiView::watch ? 2 : 0) - show_download - show_upload;
        const auto reserve_line = [&](bool present) {
            if (present && (!live || spare_rows > 0)) {
                --spare_rows;
                return true;
            }
            return false;
        };
        const bool show_context = reserve_line(context_.has_value());
        const bool show_summary = reserve_line(summary_.has_value());
        const bool show_cloud = reserve_line(delta_items_ != 0);
        const bool show_incomplete = reserve_line(
            monitoring_ && phase_ == events::SyncPhase::waiting &&
            round_incomplete_ && (!live || rows_available >= 18)
        );
        const int activity_rows =
            messages_.empty() ? 0 : std::clamp(spare_rows, 0, 2);
        spare_rows -= activity_rows;
        const auto compact_transfer = [&](bool unknown) {
            const int extra = unknown ? 3 : 4;
            if (live && spare_rows < extra) {
                return true;
            }
            spare_rows -= extra;
            return false;
        };
        Elements content{
            header(),
            separator(),
        };

        if (show_cloud) {
            content.push_back(cloud_status());
        }
        if (show_download) {
            content.push_back(transfer_status(
                *download_,
                download_->downloaded,
                translate_text("DOWNLOADS", language_),
                compact_transfer(
                    download_->total == 0 && download_->file_count == 0 &&
                    download_->state == util::ProgressState::ongoing
                )
            ));
        }
        if (show_upload) {
            content.push_back(transfer_status(
                *upload_,
                upload_->uploaded,
                translate_text("UPLOADS", language_),
                compact_transfer(
                    upload_->total == 0 && upload_->file_count == 0 &&
                    upload_->state == util::ProgressState::ongoing
                )
            ));
        }
        if (show_context && context_) {
            content.push_back(text(format_tui_sync_context(
                context_->direction, monitoring_, context_->preview, language_
            )));
        }
        if (phase_) {
            content.push_back(
                text(format_tui_sync_phase(monitoring_, *phase_, language_))
            );
        }
        if (show_summary && summary_) {
            content.push_back(
                text(format_tui_sync_summary(*summary_, language_))
            );
        }
        if (show_incomplete) {
            content.push_back(text(std::string{translate_text(
                "Last synchronization round incomplete.", language_
            )}));
        }
        if (!file_history_.entries().empty() && (!live || spare_rows >= 2)) {
            Elements files{
                emphasized(text(format_tui_file_history_heading(
                    file_history_.entries().size(),
                    file_history_.omitted(),
                    language_
                ))),
            };
            const auto visible = std::min(
                file_history_.entries().size(),
                static_cast<std::size_t>(
                    live ? std::min(
                               spare_rows - 1, std::max(1, rows_available / 5)
                           )
                         : std::max(1, rows_available / 5)
                )
            );
            for (auto index = file_history_.entries().size() - visible;
                 index < file_history_.entries().size();
                 ++index) {
                files.push_back(text(
                    format_file_operation(
                        file_history_.entries()[index], language_
                    )
                ));
            }
            auto history = vbox(std::move(files));
            content.push_back(
                live ? std::move(history) : std::move(history) | border
            );
            spare_rows -= static_cast<int>(visible) + 1;
        }
        const bool summary_with_items =
            (view_ == TuiView::blocked &&
             section_.event == "blocked_item_summary") ||
            (view_ == TuiView::files &&
             section_.event == "downloaded_file_summary") ||
            (view_ == TuiView::verify &&
             section_.event == "file_verification_summary");
        const bool paged_list =
            view_ != TuiView::partials && !result_pages_.empty() &&
            (view_ == TuiView::drives || view_ == TuiView::blocked ||
             view_ == TuiView::files || view_ == TuiView::verify ||
             result_pages_.size() > 1);
        const auto& section =
            paged_list ? result_pages_[result_index_] : section_;
        if (summary_with_items && paged_list) {
            for (const auto& field : section_.fields) {
                content.push_back(text(field.label + " " + field.value));
            }
        }
        const bool partial_list =
            view_ == TuiView::partials && !result_pages_.empty() && finished_;
        if (partial_list) {
            content.push_back(partial_results(rows_available) | flex);
        } else if (!section.title.empty() &&
                   (!live ||
                    (!section.fields.empty() &&
                     spare_rows >=
                         static_cast<int>(section.fields.size()) + 1))) {
            Elements fields{
                emphasized(text(
                    !paged_list
                        ? section.title
                        : format_tui_paged_section_title(
                              section.title,
                              view_,
                              result_index_ + 1,
                              result_pages_.size(),
                              language_
                          )
                )),
            };
            for (const auto& field : section.fields) {
                fields.push_back(text(
                    field.label + " " + field.value
                ));
            }
            auto details = vbox(std::move(fields));
            content.push_back(
                live ? std::move(details)
                     : std::move(details) | border | yflex_shrink
            );
            spare_rows -= static_cast<int>(section.fields.size()) + 1;
        }
        if (blocked_items_ != 0) {
            content.push_back(with_color(
                text(format_tui_needs_attention(
                    blocked_items_, last_blocked_, language_
                )),
                palette_.warning
            ));
        }

        if (!partial_list) {
            content.push_back(filler());
        }
        if (!messages_.empty() && (!live || activity_rows > 0)) {
            if (!live || activity_rows >= 2) {
                content.push_back(with_color(
                    emphasized(text(
                        std::string{translate_text("ACTIVITY", language_)}
                    )),
                    palette_.primary
                ));
                if (!live) {
                    content.push_back(separator());
                }
            }
            const auto message_limit =
                live ? static_cast<std::size_t>(std::max(1, spare_rows + 1))
                     : messages_.size();
            const auto first =
                file_history_.entries().empty()
                    ? messages_.size() -
                          std::min(messages_.size(), message_limit)
                    : messages_.size() -
                          std::min(
                              {messages_.size(), std::size_t{2}, message_limit}
                          );
            for (auto index = first; index < messages_.size(); ++index) {
                const auto& message = messages_[index];
                content.push_back(with_color(
                    live ? text(message.text) : paragraph(message.text),
                    color_for(message.kind)
                ));
            }
        }
        if (view_ == TuiView::watch) {
            content.push_back(separator());
            content.push_back(with_color(
                text(std::string{translate_text(" q / Esc  EXIT ", language_)}),
                palette_.accent
            ));
        }
        if (finished_) {
            content.push_back(separator());
            if (result_pages_.size() > 1) {
                const auto hint =
                    partial_list
                        ? " Up/Down or k/j: select file "
                    : view_ == TuiView::drives
                        ? " Left/Right or n/p: previous/next Drive "
                        : " Left/Right or n/p: previous/next item ";
                content.push_back(with_color(
                    text(std::string{translate_text(hint, language_)}),
                    palette_.accent
                ));
            }
            content.push_back(with_color(
                text(std::string{translate_text(
                    " Press Enter to exit (or q) ", language_
                )}),
                palette_.accent
            ));
        }
        return vbox(std::move(content)) | border;
    }

    void enter_fullscreen() {
        fullscreen_active_ = true;
        output_ << "\033[?1049h\033[2J\033[H\033[?25l";
    }

    void leave_fullscreen() noexcept {
        if (!fullscreen_active_) {
            return;
        }
        output_ << "\033[?25h\033[?1049l" << std::flush;
        fullscreen_active_ = false;
    }

    void render() {
        auto document = dashboard(static_cast<int>(rows_));
        auto screen = Screen::Create(
            Dimension::Fixed(static_cast<int>(columns_)),
            Dimension::Fixed(static_cast<int>(rows_))
        );
        Render(screen, document);
        output_ << "\033[H\033[?25l" << screen.ToString()
                << std::flush;
    }

    std::ostream& output_;
    std::size_t columns_;
    std::size_t rows_;
    Language language_{Language::english};
    TuiTheme theme_;
    TuiView view_;
    ThemePalette palette_;
    bool styled_;
    bool fullscreen_active_{false};
    bool finished_{false};
    std::vector<DashboardMessage> messages_;
    SectionEvent section_;
    std::vector<SectionEvent> result_pages_;
    std::size_t result_index_{0};
    std::optional<SectionEvent> partial_summary_;
    std::size_t delta_items_{0};
    std::size_t delta_changes_{0};
    bool delta_complete_{false};
    std::size_t blocked_items_{0};
    std::string last_blocked_;
    std::optional<DownloadProgressEvent> download_;
    std::optional<UploadProgressEvent> upload_;
    std::optional<events::SyncPhase> phase_;
    std::optional<SyncSummaryEvent> summary_;
    bool monitoring_{false};
    bool round_incomplete_{false};
    std::optional<events::SyncContextEvent> context_;
    events::FileHistory file_history_;
    bool history_printed_{false};
};

}  // namespace

std::unique_ptr<ConsoleBackend> make_ftxui_console_backend(
    ConsoleOptions options,
    std::ostream& output,
    std::ostream& error,
    std::size_t columns,
    std::size_t rows
) {
    return std::make_unique<FtxuiConsoleBackend>(
        options, output, error, columns, rows
    );
}

}  // namespace onedrive::cli::detail
