#include "ui/cli/backend_factory.hpp"
#include "ui/cli/format.hpp"
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

void replace_all(
    std::string& text,
    std::string_view search,
    std::string_view replacement
) {
    std::size_t position = 0;
    while ((position = text.find(search, position)) != std::string::npos) {
        text.replace(position, search.size(), replacement);
        position += replacement.size();
    }
}

std::string friendly_text(std::string text) {
    replace_all(text, "Microsoft Graph", "cloud service");
    replace_all(text, "Graph", "cloud service");
    replace_all(text, "WebSocket", "live cloud updates");
    replace_all(text, "Delta", "cloud check");
    replace_all(text, "delta", "cloud check");
    return text;
}

std::optional<std::string> friendly_message(
    const MessageEvent& event
) {
    if (event.event == "log" &&
        event.kind == MessageKind::information) {
        return std::nullopt;
    }
    if (event.event == "delta_query_started") {
        return "Checking the cloud for changes...";
    }
    if (event.event == "delta_cursor_invalid") {
        return "Refreshing cloud history...";
    }
    if (event.event == "sync_completed") {
        return "Sync complete.";
    }
    if (event.event == "sync_completed_with_issues") {
        return "Sync finished with items needing attention.";
    }
    if (event.event == "initial_delta_scheduled") {
        return "The next sync will perform a full cloud check.";
    }

    auto text = event.text;
    replace_all(
        text,
        "Local changes settle for ",
        "Local changes wait "
    );
    replace_all(
        text,
        " milliseconds; remote WebSocket notifications trigger Delta "
        "synchronization, with Graph polling every ",
        " milliseconds to settle; live cloud updates are enabled, with a "
        "safety check every "
    );
    replace_all(
        text,
        " milliseconds; remote WebSocket notifications are disabled, and "
        "Graph is polled every ",
        " milliseconds to settle; live cloud updates are disabled, so "
        "changes are checked every "
    );
    replace_all(text, " seconds as fallback.", " seconds.");
    replace_all(
        text,
        "Monitoring local and Microsoft Graph changes for:",
        "Watching for local and cloud changes in:"
    );
    replace_all(text, "Remote delta contains", "Cloud storage has");
    replace_all(text, " upserts", " new or updated");
    replace_all(text, " removals", " removed");
    replace_all(text, " moves", " moved");
    replace_all(text, " blocked", " need attention");
    replace_all(text, "the Delta cursor", "cloud change history");
    return friendly_text(std::move(text));
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
          theme_{options.theme},
          view_{options.view},
          palette_{palette_for(theme_)},
          styled_{
              options.color != ColorMode::never &&
              (options.color == ColorMode::always ||
               std::getenv("NO_COLOR") == nullptr)
          } {
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

    void finish() override {
        switch (view_) {
            case TuiView::health:
            case TuiView::status:
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

    bool confirm(const ConfirmationRequest& request) override {
        output_ << "\033[2J\033[H\033[?25h"
                << request.prompt << std::flush;
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
            case MessageKind::information:
                return palette_.accent;
            case MessageKind::success:
                return palette_.success;
            case MessageKind::warning:
                return palette_.warning;
            case MessageKind::error:
                return palette_.error;
        }
        throw_unknown_message_kind();
    }

    void update(const MessageEvent& event) {
        auto message = friendly_message(event);
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
        section_.title = friendly_text(event.title);
        for (auto& field : section_.fields) {
            field.label = friendly_text(std::move(field.label));
        }
        if ((view_ == TuiView::drives && event.event == "drive") ||
            (view_ == TuiView::partials && event.event == "partial_download")) {
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
        last_blocked_ =
            event.path + ": " + friendly_text(event.reason_message);
    }

    void update(const DownloadProgressEvent& event) {
        download_ = event;
    }

    void update(const EndDownloadProgressEvent&) {}

    Element header() const {
        const auto view_name = [&] {
            switch (view_) {
                case TuiView::auth:
                    return "AUTH";
                case TuiView::health:
                    return "HEALTH";
                case TuiView::status:
                    return "STATUS";
                case TuiView::drives:
                    return "DRIVES";
                case TuiView::shared:
                    return "SHARED";
                case TuiView::sites:
                    return "SITES";
                case TuiView::quota:
                    return "QUOTA";
                case TuiView::storage:
                    return "STORAGE";
                case TuiView::partials:
                    return "PARTIALS";
                case TuiView::files:
                    return "FILES";
                case TuiView::verify:
                    return "VERIFY";
                case TuiView::config:
                    return "CONFIG";
                case TuiView::download:
                    return "DOWNLOAD";
                case TuiView::watch:
                    return "WATCH";
                case TuiView::sync:
                    return "SYNC";
            }
            return "SYNC";
        }();
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
        auto status = fmt::format(
            "CLOUD CHECK  {} items checked", delta_items_
        );
        if (delta_complete_) {
            status += fmt::format(
                "  //  {} changes found", delta_changes_
            );
        }
        return with_color(
            text(std::move(status)),
            delta_complete_ ? palette_.success : palette_.accent
        );
    }

    Element download_status() const {
        const auto percentage = download_progress_percentage(
            download_->completed_files,
            download_->file_count,
            download_->downloaded,
            download_->total,
            download_->state
        );
        auto progress = gauge(
            static_cast<float>(percentage) / 100.0F
        );
        progress = with_color(std::move(progress), palette_.primary);
        auto details = fmt::format(
            "{}%  {} / {}  {}/s",
            percentage,
            format_bytes(download_->downloaded),
            format_bytes(download_->total),
            format_bytes(download_->metrics.bytes_per_second)
        );
        if (download_->state == util::ProgressState::completed) {
            details += fmt::format(
                "  elapsed {}",
                download_->metrics.elapsed_milliseconds < 1000 ?
                    "<1s" :
                    format_duration(
                        download_->metrics.elapsed_milliseconds / 1000
                    )
            );
        } else {
            details += fmt::format(
                "  ETA {}",
                download_->metrics.estimated_seconds_remaining.has_value() ?
                    format_duration(
                        *download_->metrics.estimated_seconds_remaining
                    ) :
                    "calculating..."
            );
        }
        return vbox({
            emphasized(text(fmt::format(
                "DOWNLOADS  {}/{} files",
                download_->completed_files,
                download_->file_count
            ))),
            hbox({
                text("["),
                std::move(progress) | flex,
                text("]"),
            }),
            text(std::move(details)),
        }) | border;
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
        content.push_back(emphasized(text(
            fmt::format(
                "Partial downloads [File {}/{}]",
                result_index_ + 1,
                result_pages_.size()
            )
        )));
        content.push_back(hbox({
            text("  PATH") | xflex,
            text("STATUS  SAVED PROGRESS / TOTAL"),
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
        Elements content{
            header(),
            separator(),
        };

        if (delta_items_ != 0) {
            content.push_back(cloud_status());
        }
        if (download_) {
            content.push_back(download_status());
        }
        const bool drive_list =
            view_ == TuiView::drives && !result_pages_.empty();
        const auto& section =
            drive_list ? result_pages_[result_index_] : section_;
        const bool partial_list =
            view_ == TuiView::partials && !result_pages_.empty() && finished_;
        if (partial_list) {
            content.push_back(partial_results(rows_available) | flex);
        } else if (!section.title.empty()) {
            Elements fields{
                emphasized(text(
                    !drive_list ? section.title
                                : fmt::format(
                                      "{}  [Drive {}/{}]",
                                      section.title,
                                      result_index_ + 1,
                                      result_pages_.size()
                                  )
                )),
            };
            for (const auto& field : section.fields) {
                fields.push_back(text(
                    field.label + " " + field.value
                ));
            }
            content.push_back(vbox(std::move(fields)) | border | yflex_shrink);
        }
        if (blocked_items_ != 0) {
            content.push_back(with_color(
                text(fmt::format(
                    "NEEDS ATTENTION  {}  //  {}",
                    blocked_items_,
                    last_blocked_
                )),
                palette_.warning
            ));
        }

        if (!partial_list) {
            content.push_back(filler());
        }
        if (!messages_.empty()) {
            content.push_back(with_color(
                emphasized(text("ACTIVITY")),
                palette_.primary
            ));
            content.push_back(separator());
            for (const auto& message : messages_) {
                content.push_back(with_color(
                    paragraph(message.text), color_for(message.kind)
                ));
            }
        }
        if (view_ == TuiView::watch) {
            content.push_back(separator());
            content.push_back(with_color(
                text(" q / Esc  EXIT "),
                palette_.accent
            ));
        }
        if (finished_) {
            content.push_back(separator());
            if (result_pages_.size() > 1) {
                content.push_back(with_color(
                    text(
                        partial_list
                            ? " Up/Down or k/j: select file "
                            : " Left/Right or n/p: previous/next Drive "
                    ),
                    palette_.accent
                ));
            }
            content.push_back(with_color(
                text(" Press Enter to exit (or q) "), palette_.accent
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
