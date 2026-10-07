#include "cli/backend_factory.hpp"
#include "cli/format.hpp"
#include "cli/message.hpp"

#include "onedrive/cli/console.hpp"
#include "onedrive/version.hpp"

#include <fmt/format.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
        section_title_ = friendly_text(event.title);
        section_fields_ = event.fields;
        for (auto& field : section_fields_) {
            field.label = friendly_text(std::move(field.label));
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
                case TuiView::doctor:
                    return "DOCTOR";
                case TuiView::status:
                    return "STATUS";
                case TuiView::download:
                    return "DOWNLOAD";
                case TuiView::monitor:
                    return "MONITOR";
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

    Element dashboard() const {
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
        if (!section_title_.empty()) {
            Elements fields{
                emphasized(text(section_title_)),
            };
            for (const auto& field : section_fields_) {
                fields.push_back(text(
                    field.label + " " + field.value
                ));
            }
            content.push_back(vbox(std::move(fields)) | border);
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

        content.push_back(filler());
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
        if (view_ == TuiView::monitor) {
            content.push_back(separator());
            content.push_back(with_color(
                text(" q / Esc  EXIT "),
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
        auto document = dashboard();
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
    std::vector<DashboardMessage> messages_;
    std::string section_title_;
    std::vector<Field> section_fields_;
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
