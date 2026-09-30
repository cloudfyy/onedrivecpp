#include "onedrive/app/application.hpp"

#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "onedrive/storage/item_database.hpp"
#include "onedrive/sync/sync_engine.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace onedrive::app {
namespace {

constexpr std::string_view version{"0.1.0"};

std::filesystem::path default_config_path() {
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        return std::filesystem::path{home} / ".config/onedrive-cpp/config";
    }
    return "/etc/onedrive-cpp/onedrive-cpp.conf";
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
        std::cout << "onedrive-cpp " << version << '\n';
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
        << "  " << program << " sync [--config PATH] [--dry-run]\n"
        << "  " << program << " monitor [--config PATH]\n"
        << "  " << program << " --version\n\n"
        << "The current milestone provides the architecture and local dry-run flow.\n";
}

}  // namespace onedrive::app
