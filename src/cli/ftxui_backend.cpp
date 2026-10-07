#include "cli/backend_factory.hpp"
#include "cli/format.hpp"

#include "onedrive/cli/console.hpp"

#include <fmt/format.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace onedrive::cli::detail {
namespace {

using namespace ftxui;

constexpr std::size_t maximum_messages = 6;

Color color_for(MessageKind kind) {
    switch (kind) {
        case MessageKind::information:
            return Color::Cyan;
        case MessageKind::success:
            return Color::Green;
        case MessageKind::warning:
            return Color::Yellow;
        case MessageKind::error:
            return Color::Red;
    }
    return Color::Default;
}

class FtxuiConsoleBackend final : public ConsoleBackend {
public:
    FtxuiConsoleBackend(
        ConsoleOptions options,
        std::ostream& output,
        std::ostream&,
        std::size_t columns
    )
        : output_{output},
          columns_{columns},
          styled_{
              options.color != ColorMode::never &&
              (options.color == ColorMode::always ||
               std::getenv("NO_COLOR") == nullptr)
          } {}

    ~FtxuiConsoleBackend() override {
        if (rendered_lines_ != 0) {
            output_ << "\033[?25h" << std::flush;
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
        clear();
        output_ << "\033[?25h" << request.prompt << std::flush;
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

    void update(const MessageEvent& event) {
        messages_.push_back({event.kind, event.text});
        if (messages_.size() > maximum_messages) {
            messages_.erase(messages_.begin());
        }
    }

    void update(const SectionEvent& event) {
        section_title_ = event.title;
        section_fields_ = event.fields;
    }

    void update(const DeltaProgressEvent& event) {
        delta_pages_ = event.pages;
        delta_items_ = event.items;
        delta_complete_ =
            event.state == util::ProgressState::completed;
    }

    void update(const DeltaSummaryEvent& event) {
        delta_pages_ = event.summary.pages;
        delta_items_ = event.summary.scanned_items;
        delta_changes_ = event.summary.unique_changes;
        delta_complete_ = true;
    }

    void update(const BlockedItemEvent& event) {
        ++blocked_items_;
        last_blocked_ = event.path + ": " + event.reason_message;
    }

    void update(const DownloadProgressEvent& event) {
        download_ = event;
    }

    void update(const EndDownloadProgressEvent&) {
        download_.reset();
    }

    Element dashboard() const {
        Elements content{
            with_color(
                emphasized(text("onedrive-cpp sync")), Color::Blue
            ),
            separator(),
        };

        if (delta_pages_ != 0 || delta_items_ != 0) {
            auto delta = fmt::format(
                "Microsoft Graph: {} page{}, {} items",
                delta_pages_,
                delta_pages_ == 1 ? "" : "s",
                delta_items_
            );
            if (delta_complete_) {
                delta += fmt::format(
                    ", {} unique changes", delta_changes_
                );
            }
            content.push_back(with_color(
                text(std::move(delta)),
                delta_complete_ ? Color::Green : Color::Cyan
            ));
        }

        if (download_) {
            const auto percentage = download_progress_percentage(
                download_->completed_files,
                download_->file_count,
                download_->downloaded,
                download_->total,
                download_->state
            );
            auto progress =
                gauge(static_cast<float>(percentage) / 100.0F);
            progress = with_color(std::move(progress), Color::Blue);
            content.push_back(vbox({
                text(fmt::format(
                    "Downloads: {}/{} files",
                    download_->completed_files,
                    download_->file_count
                )),
                std::move(progress),
                text(fmt::format(
                    "{}%  {} / {}  {}/s",
                    percentage,
                    format_bytes(download_->downloaded),
                    format_bytes(download_->total),
                    format_bytes(download_->metrics.bytes_per_second)
                )),
            }) | border);
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
                    "Blocked: {}  {}", blocked_items_, last_blocked_
                )),
                Color::Yellow
            ));
        }

        if (!messages_.empty()) {
            content.push_back(separator());
            for (const auto& message : messages_) {
                content.push_back(with_color(
                    text(message.text), color_for(message.kind)
                ));
            }
        }
        return vbox(std::move(content)) | border;
    }

    void clear() {
        if (rendered_lines_ == 0) {
            return;
        }
        output_ << fmt::format("\033[{}F\033[J", rendered_lines_);
        rendered_lines_ = 0;
    }

    void render() {
        clear();
        auto document = dashboard();
        auto screen = Screen::Create(
            Dimension::Fixed(static_cast<int>(columns_)),
            Dimension::Fit(document)
        );
        Render(screen, document);
        output_ << "\033[?25l" << screen.ToString() << '\n'
                << std::flush;
        rendered_lines_ = static_cast<std::size_t>(screen.dimy());
    }

    std::ostream& output_;
    std::size_t columns_;
    bool styled_;
    std::size_t rendered_lines_{0};
    std::vector<DashboardMessage> messages_;
    std::string section_title_;
    std::vector<Field> section_fields_;
    std::size_t delta_pages_{0};
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
    std::size_t columns
) {
    return std::make_unique<FtxuiConsoleBackend>(
        options, output, error, columns
    );
}

}  // namespace onedrive::cli::detail
