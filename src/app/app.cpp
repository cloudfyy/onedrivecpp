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
    const cli::Console console{
        {
            .color = arguments.color_mode,
            .output = arguments.output_mode,
            .quiet = arguments.quiet,
        }
    };

    std::optional<logging::Session> logging_session;
    try {
        logging_session.emplace(
            logging::Options{
                .level = arguments.log_level,
                .file = arguments.log_file.empty() ?
                            std::nullopt :
                            std::optional<std::filesystem::path>{
                                arguments.log_file
                            },
            }
        );
        auto config = config::Config::load(arguments.config_path);
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
