#include "onedrive/app/application.hpp"

#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/http/http_client.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "onedrive/storage/item_database.hpp"
#include "onedrive/sync/sync_engine.hpp"
#include "onedrive/version.hpp"

#include <CLI/CLI.hpp>

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

int authenticate(const config::Config& config) {
    http::CurlHttpClient transport;
    auth::DeviceAuthClient client{
        transport,
        auth::DeviceAuthOptions{
            .application_id = config.application_id,
            .tenant_id = config.azure_tenant_id,
            .auth_endpoint = config.auth_endpoint,
            .scope = config.auth_scope,
        },
    };

    auto device_code = client.request_device_code();
    if (!device_code) {
        std::cerr << "Authentication failed: " << device_code.error().message << '\n';
        return 1;
    }

    if (!device_code->message.empty()) {
        std::cout << device_code->message << '\n';
    } else {
        std::cout << "Open " << device_code->verification_uri
                  << " and enter code " << device_code->user_code << '\n';
    }
    std::cout << "Waiting for authorization...\n";

    auto tokens = client.poll_for_token(*device_code);
    if (!tokens) {
        std::cerr << "Authentication failed: " << tokens.error().message << '\n';
        return 1;
    }

    auth::TokenStore token_store{config.state_directory};
    token_store.save_refresh_token(tokens->refresh_token);
    std::cout << "Authentication succeeded. Refresh token saved to "
              << token_store.path() << '\n';
    return 0;
}

}  // namespace

int Application::run(int argc, char* argv[]) {
    std::filesystem::path config_path = default_config_path();
    bool force_dry_run = false;

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
    auto* sync_command = cli.add_subcommand("sync", "Synchronize OneDrive files");
    auto* monitor_command =
        cli.add_subcommand("monitor", "Monitor for synchronization changes");

    const auto add_config_option = [&config_path](CLI::App& command) {
        command
            .add_option("--config", config_path, "Path to the configuration file")
            ->type_name("PATH");
    };
    add_config_option(*auth_command);
    add_config_option(*logout_command);
    add_config_option(*sync_command);
    add_config_option(*monitor_command);
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

    try {
        auto config = config::Config::load(config_path);
        config.dry_run = config.dry_run || force_dry_run;

        if (*auth_command) {
            return authenticate(config);
        }
        if (*logout_command) {
            const bool removed =
                auth::TokenStore{config.state_directory}.remove_refresh_token();
            std::cout << (removed ? "Saved authentication removed.\n" :
                                   "No saved authentication was present.\n");
            return 0;
        }
        if (*monitor_command) {
            return monitor::Monitor{config.sync_directory}.run();
        }

        storage::ItemDatabase database{config.state_directory};
        database.open();
        graph::GraphClient graph;
        return sync::SyncEngine{config, graph, database}.synchronize();
    } catch (const std::exception& error) {
        std::cerr << "onedrive-cpp: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace onedrive::app
