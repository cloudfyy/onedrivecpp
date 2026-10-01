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
    const std::string_view program = argc > 0 ? argv[0] : "onedrive-cpp";
    if (argc < 2 || std::string_view{argv[1]} == "--help") {
        print_help(program);
        return 0;
    }

    const std::string_view command{argv[1]};
    if (command == "--version") {
        std::cout << "onedrive-cpp " << build_info::version << '\n';
        return 0;
    }

    std::filesystem::path config_path = default_config_path();
    bool force_dry_run = false;
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--dry-run") {
            force_dry_run = true;
        } else if (argument == "--config" && index + 1 < argc) {
            config_path = argv[++index];
        } else {
            std::cerr << "Unknown or incomplete option: " << argument << '\n';
            return 2;
        }
    }

    try {
        auto config = config::Config::load(config_path);
        config.dry_run = config.dry_run || force_dry_run;

        if (command == "auth") {
            return authenticate(config);
        }
        if (command == "logout") {
            const bool removed =
                auth::TokenStore{config.state_directory}.remove_refresh_token();
            std::cout << (removed ? "Saved authentication removed.\n" :
                                   "No saved authentication was present.\n");
            return 0;
        }
        if (command == "monitor") {
            return monitor::Monitor{config.sync_directory}.run();
        }
        if (command != "sync") {
            std::cerr << "Unknown command: " << command << '\n';
            print_help(program);
            return 2;
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

void Application::print_help(std::string_view program) {
    std::cout
        << "Usage:\n"
        << "  " << program << " auth [--config PATH]\n"
        << "  " << program << " logout [--config PATH]\n"
        << "  " << program << " sync [--config PATH] [--dry-run]\n"
        << "  " << program << " monitor [--config PATH]\n"
        << "  " << program << " --version\n\n"
        << "Use 'auth' to authorize with Microsoft using the device code flow.\n";
}

}  // namespace onedrive::app
