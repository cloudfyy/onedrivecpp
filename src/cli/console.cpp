#include "onedrive/cli/console.hpp"

#include "cli/backend_factory.hpp"
#include "cli/terminal.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace onedrive::cli {
namespace {

constexpr unsigned completed_percentage = 100;
constexpr unsigned maximum_incomplete_percentage = 99;

std::unique_ptr<ConsoleBackend> make_backend(
    ConsoleOptions options,
    std::ostream& output,
    std::ostream& error
) {
    if (options.output == OutputMode::json) {
        if (options.ui == UiMode::tui) {
            throw std::invalid_argument{
                "--ui=tui cannot be combined with --output=json"
            };
        }
        return detail::make_json_console_backend(
            options, output, error
        );
    }
    if (options.quiet) {
        if (options.ui == UiMode::tui) {
            throw std::invalid_argument{
                "--ui=tui cannot be combined with --quiet"
            };
        }
        return detail::make_text_console_backend(options, output, error);
    }
    if (options.ui == UiMode::console) {
        return detail::make_text_console_backend(options, output, error);
    }

    auto capabilities = detail::probe_terminal();
    if (&output != &std::cout) {
        capabilities.output_is_terminal = false;
    }
    if (detail::supports_tui(capabilities)) {
        return detail::make_ftxui_console_backend(
            options, output, error, capabilities.columns
        );
    }
    if (options.ui == UiMode::tui) {
        throw std::runtime_error{
            "FTXUI is unavailable: " +
            detail::tui_unavailable_reason(capabilities)
        };
    }
    return detail::make_text_console_backend(options, output, error);
}

}  // namespace

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

Console::Console(
    ConsoleOptions options,
    std::ostream& output,
    std::ostream& error
)
    : backend_{make_backend(options, output, error)} {}

Console::Console(std::unique_ptr<ConsoleBackend> backend)
    : backend_{std::move(backend)} {
    if (!backend_) {
        throw std::invalid_argument{
            "console backend must not be null"
        };
    }
}

Console::~Console() = default;

void Console::message(
    MessageKind kind,
    std::string_view event,
    std::string_view text
) const {
    const std::scoped_lock lock{backend_mutex_};
    backend_->emit(MessageEvent{
        .kind = kind,
        .event = std::string{event},
        .text = std::string{text},
    });
}

void Console::section(
    std::string_view event,
    std::string_view title,
    const std::vector<Field>& fields
) const {
    const std::scoped_lock lock{backend_mutex_};
    backend_->emit(SectionEvent{
        .event = std::string{event},
        .title = std::string{title},
        .fields = fields,
    });
}

void Console::delta_progress(
    std::size_t pages,
    std::size_t items,
    util::ProgressState state
) const {
    const std::scoped_lock lock{backend_mutex_};
    backend_->emit(DeltaProgressEvent{
        .pages = pages,
        .items = items,
        .state = state,
    });
}

void Console::delta_summary(const DeltaSummary& summary) const {
    const std::scoped_lock lock{backend_mutex_};
    backend_->emit(DeltaSummaryEvent{.summary = summary});
}

void Console::blocked_item(
    std::string_view path,
    std::string_view reason_code,
    std::string_view reason_message
) const {
    const std::scoped_lock lock{backend_mutex_};
    backend_->emit(BlockedItemEvent{
        .path = std::string{path},
        .reason_code = std::string{reason_code},
        .reason_message = std::string{reason_message},
    });
}

void Console::download_progress(
    std::size_t completed_files,
    std::size_t file_count,
    std::uint64_t downloaded,
    std::uint64_t total,
    util::ProgressState state,
    const DownloadProgressMetrics& metrics
) const {
    const std::scoped_lock lock{backend_mutex_};
    backend_->emit(DownloadProgressEvent{
        .completed_files = completed_files,
        .file_count = file_count,
        .downloaded = downloaded,
        .total = total,
        .state = state,
        .metrics = metrics,
    });
}

void Console::end_download_progress() const {
    const std::scoped_lock lock{backend_mutex_};
    backend_->emit(EndDownloadProgressEvent{});
}

bool Console::confirm(
    std::string_view event,
    std::string_view prompt,
    std::string_view expected
) const {
    const ConfirmationRequest request{
        .event = std::string{event},
        .prompt = std::string{prompt},
        .expected = std::string{expected},
    };
    bool matched;
    {
        const std::scoped_lock lock{backend_mutex_};
        matched = backend_->confirm(request);
    }
    if (!matched) {
        message(
            MessageKind::warning,
            event,
            "Confirmation did not match."
        );
    }
    return matched;
}

OutputMode Console::output_mode() const noexcept {
    return backend_->output_mode();
}

UiMode Console::ui_mode() const noexcept {
    return backend_->ui_mode();
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

UiMode Console::parse_ui_mode(std::string_view value) {
    if (value == "auto") {
        return UiMode::automatic;
    }
    if (value == "console") {
        return UiMode::console;
    }
    if (value == "tui") {
        return UiMode::tui;
    }
    throw std::invalid_argument("invalid UI mode: " + std::string{value});
}

std::ostream& Console::default_output() {
    return std::cout;
}

std::ostream& Console::default_error() {
    return std::cerr;
}

}  // namespace onedrive::cli
