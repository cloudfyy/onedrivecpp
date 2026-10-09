#include "onedrive/ui/cli/console.hpp"

#include "ui/cli/backend_factory.hpp"
#include "ui/cli/terminal.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace onedrive::cli {
namespace {

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
            options,
            output,
            error,
            capabilities.columns,
            capabilities.rows
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

void Console::finish() const {
    const auto guard = lock();
    backend_->finish();
}

void Console::on_event(const events::Event& event) const {
    backend_->emit(event);
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
    bool matched{false};
    {
        const auto guard = lock();
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
    return config::parse_color_mode(value);
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
    return config::parse_ui_mode(value);
}

TuiTheme Console::parse_tui_theme(std::string_view value) {
    return config::parse_tui_theme(value);
}

std::ostream& Console::default_output() {
    return std::cout;
}

std::ostream& Console::default_error() {
    return std::cerr;
}

}  // namespace onedrive::cli
