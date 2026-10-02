#include "onedrive/app/application.hpp"

#include "onedrive/app/runtime_factory.hpp"
#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/http/http_client.hpp"
#include "onedrive/logging/logging.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "onedrive/storage/item_store.hpp"
#include "onedrive/sync/sync_engine.hpp"
#include "onedrive/version.hpp"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace onedrive::app {
namespace {

std::filesystem::path default_config_path() {
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        return std::filesystem::path{home} / ".config/onedrive-cpp/config";
    }
    return "/etc/onedrive-cpp/onedrive-cpp.conf";
}

int authenticate(
    const config::Config& config,
    const RuntimeFactory& runtime_factory
) {
    auto transport = runtime_factory.create_http_transport();
    auto client = runtime_factory.create_device_auth_client(config, *transport);

    auto device_code = client->request_device_code();
    if (!device_code) {
        spdlog::error(
            "Microsoft device authorization request failed: {}",
            device_code.error().message
        );
        return 1;
    }

    if (!device_code->message.empty()) {
        std::cout << device_code->message << '\n';
    } else {
        std::cout << "Open " << device_code->verification_uri
                  << " and enter code " << device_code->user_code << '\n';
    }
    std::cout << "Waiting for authorization...\n";

    auto tokens = client->poll_for_token(*device_code);
    if (!tokens) {
        spdlog::error(
            "Microsoft device authorization failed: {}",
            tokens.error().message
        );
        return 1;
    }

    auto token_store = runtime_factory.create_token_store(config);
    token_store->save_refresh_token(tokens->refresh_token);
    spdlog::info("Microsoft authentication succeeded");
    std::cout << "Authentication succeeded. Refresh token saved to "
              << token_store->path() << '\n';
    return 0;
}

}  // namespace

Application::Application(const RuntimeFactory& runtime_factory)
    : runtime_factory_{runtime_factory} {}

int Application::run(int argc, char* argv[]) {
    std::filesystem::path config_path = default_config_path();
    bool force_dry_run = false;
    std::string log_level{"info"};
    std::string log_file;

    CLI::App cli{
        "A modern C++ OneDrive synchronization client",
        "onedrive-cpp"
    };
    cli.set_version_flag(
        "--version",
        std::string{"onedrive-cpp "} + build_info::version
    );
    cli.require_subcommand(1);

    auto* auth_command = cli.add_subcommand(
        "auth",
        "Authorize with Microsoft using the device code flow"
    );
    auto* logout_command = cli.add_subcommand(
        "logout",
        "Remove the locally stored refresh token"
    );
    auto* reset_state_command = cli.add_subcommand(
        "reset-state",
        "Reset persisted synchronization state for the configured drive"
    );
    auto* sync_command = cli.add_subcommand("sync", "Synchronize OneDrive files");
    auto* monitor_command =
        cli.add_subcommand("monitor", "Monitor for synchronization changes");

    const auto add_common_options =
        [&config_path, &log_level, &log_file](CLI::App& command) {
        command
            .add_option("--config", config_path, "Path to the configuration file")
            ->type_name("PATH");
        command
            .add_option("--log-level", log_level, "Minimum log level")
            ->check(CLI::IsMember(
                {"trace", "debug", "info", "warn", "error", "critical", "off"},
                CLI::ignore_case
            ))
            ->capture_default_str();
        command
            .add_option("--log-file", log_file, "Also write rotating logs to this file")
            ->type_name("PATH");
    };
    add_common_options(*auth_command);
    add_common_options(*logout_command);
    add_common_options(*reset_state_command);
    add_common_options(*sync_command);
    add_common_options(*monitor_command);
    sync_command->add_flag(
        "--dry-run",
        force_dry_run,
        "Show synchronization inputs without changing remote files"
    );

    if (argc < 2) {
        std::cout << cli.help();
        return 0;
    }

    try {
        cli.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        const int exit_code = cli.exit(error);
        return exit_code == 0 ? 0 : 2;
    }

    std::optional<logging::Session> logging_session;
    try {
        logging_session.emplace(
            logging::Options{
                .level = log_level,
                .file = log_file.empty() ?
                            std::nullopt :
                            std::optional<std::filesystem::path>{log_file},
            }
        );
        auto config = config::Config::load(config_path);
        config.dry_run = config.dry_run || force_dry_run;

        if (*auth_command) {
            spdlog::info("Starting Microsoft authentication");
            return authenticate(config, runtime_factory_);
        }
        if (*logout_command) {
            spdlog::info("Removing locally saved authentication");
            const bool removed =
                runtime_factory_.create_token_store(config)->remove_refresh_token();
            std::cout << (removed ? "Saved authentication removed.\n" :
                                   "No saved authentication was present.\n");
            return 0;
        }
        if (*reset_state_command) {
            spdlog::info(
                "Resetting synchronization state for drive '{}'",
                config.drive_id
            );
            auto items = runtime_factory_.create_item_store(config);
            items->open();
            const auto removed = items->reset(config.drive_id);
            spdlog::info(
                "Synchronization state reset completed for drive '{}': {} items "
                "removed; next sync will use an initial delta query",
                config.drive_id,
                removed
            );
            std::cout << "Reset synchronization state for drive '"
                      << config.drive_id << "': " << removed
                      << " items removed.\n"
                      << "The next sync will perform a full Microsoft Graph "
                         "delta query.\n";
            return 0;
        }
        if (*monitor_command) {
            spdlog::info("Starting filesystem monitor");
            return runtime_factory_.create_monitor(config)->run();
        }

        spdlog::info("Starting synchronization{}", config.dry_run ? " dry run" : "");
        auto items = runtime_factory_.create_item_store(config);
        items->open();
        auto graph = runtime_factory_.create_graph_client(config);
        auto metrics = runtime_factory_.create_metrics();
        return sync::SyncEngine{config, *graph, *items, *metrics}.synchronize();
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
