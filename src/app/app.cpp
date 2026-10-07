#include "onedrive/app/app.hpp"

#include "args.hpp"
#include "commands.hpp"
#include "monitor/signal.hpp"
#include "onedrive/logging/logging.hpp"
#include "preflight.hpp"

#include <spdlog/spdlog.h>

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <utility>

namespace onedrive::app {
namespace {

cli::MessageKind message_kind(logging::Severity severity) noexcept {
    switch (severity) {
        case logging::Severity::trace:
        case logging::Severity::debug:
        case logging::Severity::information:
            return cli::MessageKind::information;
        case logging::Severity::warning:
            return cli::MessageKind::warning;
        case logging::Severity::error:
        case logging::Severity::critical:
            return cli::MessageKind::error;
    }
    return cli::MessageKind::information;
}

cli::TuiView tui_view(detail::Operation operation) noexcept {
    switch (operation) {
        case detail::Operation::authenticate:
            return cli::TuiView::auth;
        case detail::Operation::diagnose:
            return cli::TuiView::health;
        case detail::Operation::status:
            return cli::TuiView::status;
        case detail::Operation::drives:
            return cli::TuiView::drives;
        case detail::Operation::shared:
            return cli::TuiView::shared;
        case detail::Operation::sites:
            return cli::TuiView::sites;
        case detail::Operation::quota:
            return cli::TuiView::quota;
        case detail::Operation::storage:
            return cli::TuiView::storage;
        case detail::Operation::partials:
            return cli::TuiView::partials;
        case detail::Operation::files:
            return cli::TuiView::files;
        case detail::Operation::verify:
            return cli::TuiView::verify;
        case detail::Operation::config:
            return cli::TuiView::config;
        case detail::Operation::download:
            return cli::TuiView::download;
        case detail::Operation::monitor:
            return cli::TuiView::watch;
        default:
            return cli::TuiView::sync;
    }
}

}  // namespace

Application::Application(
    gsl::not_null<const RuntimeFactory*> runtime_factory
)
    : runtime_factory_{runtime_factory} {}

int Application::run(int argc, char* argv[]) {
    auto parsed = detail::parse_arguments(argc, argv);
    if (parsed.exit_code) {
        return *parsed.exit_code;
    }
    const auto& arguments = parsed.arguments;
    try {
        auto config = config::Config::load(arguments.config_path);
        const cli::Console console{
            {
                .color = arguments.color_mode.value_or(config.console_color),
                .output = arguments.output_mode,
                .ui = detail::operation_capabilities(arguments.operation)
                              .supports_tui ?
                    arguments.ui_mode.value_or(config.console_ui) :
                    cli::UiMode::console,
                .theme =
                    arguments.tui_theme.value_or(config.console_theme),
                .view = tui_view(arguments.operation),
                .quiet = arguments.quiet,
            }
        };
        const logging::Session logging_session{
            {
                .level = arguments.log_level.value_or(config.logging.level),
                .file = arguments.log_file ?
                    std::optional<std::filesystem::path>{*arguments.log_file} :
                    config.logging.file,
                .message_sink = console.ui_mode() == cli::UiMode::tui ?
                    logging::MessageSink{
                        [&console](
                            logging::Severity severity,
                            std::string_view message
                        ) {
                            console.message(
                                message_kind(severity), "log", message
                            );
                        }
                    } :
                    logging::MessageSink{},
            }
        };
        config.dry_run = config.dry_run || arguments.force_dry_run;
        config.force_large_delete = arguments.force_large_delete;

        std::optional<monitor::detail::TerminationSignalMask>
            monitor_signal_mask;
        if (arguments.operation == detail::Operation::monitor) {
            monitor_signal_mask.emplace();
        }
        const detail::RuntimePreflight runtime_preflight{
            config, arguments.operation
        };
        return detail::execute_command(
            arguments, std::move(config), *runtime_factory_, console
        );
    } catch (const std::exception& error) {
        if (const auto logger = spdlog::default_logger()) {
            logger->error("{}", error.what());
        } else {
            std::cerr << "onedrive-cpp: " << error.what() << '\n';
        }
        return 1;
    }
}

}  // namespace onedrive::app
