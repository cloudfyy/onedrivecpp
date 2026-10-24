#pragma once

#include "onedrive/ui/cli/app.hpp"

#include "fake_auth.hpp"
#include "fake_graph.hpp"
#include "fake_items.hpp"
#include "fake_runtime.hpp"
#include "onedrive/app/factory.hpp"
#include "onedrive/app/options.hpp"
#include "onedrive/account/account_state.hpp"
#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/http/http_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "onedrive/storage/item_database.hpp"
#include "onedrive/storage/item_store.hpp"
#include "support/sync.hpp"
#include "support/common.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace onedrive::test::app {

using onedrive::test::TemporaryDirectory;

struct RunResult {
    int exit_code;
    std::string standard_output;
    std::string standard_error;
};

inline RunResult run_application(
    FakeRuntimeFactory& runtime_factory,
    std::vector<std::string> arguments,
    std::string standard_input = {}
) {
    std::vector<char*> argument_pointers;
    argument_pointers.reserve(arguments.size());
    for (auto& argument : arguments) {
        argument_pointers.push_back(argument.data());
    }

    std::ostringstream standard_output;
    std::ostringstream standard_error;
    std::istringstream input{std::move(standard_input)};
    auto* original_input = std::cin.rdbuf(input.rdbuf());
    auto* original_output = std::cout.rdbuf(standard_output.rdbuf());
    auto* original_error = std::cerr.rdbuf(standard_error.rdbuf());

    onedrive::app::RuntimeFactory runtime_factory_proxy{
        onedrive::util::borrowed_proxy, runtime_factory
    };
    onedrive::app::Application application{&runtime_factory_proxy};
    const int exit_code = application.run(
        static_cast<int>(argument_pointers.size()), argument_pointers.data()
    );

    std::cout.rdbuf(original_output);
    std::cerr.rdbuf(original_error);
    std::cin.rdbuf(original_input);
    return {
        .exit_code = exit_code,
        .standard_output = std::move(standard_output).str(),
        .standard_error = std::move(standard_error).str(),
    };
}

struct CliFixture final {
    TemporaryDirectory temporary_directory;
    std::filesystem::path config_path{
        temporary_directory.path() / "config.toml"
    };
    std::filesystem::path state_path{temporary_directory.path() / "state"};
    std::filesystem::path sync_path{temporary_directory.path() / "files"};
    std::filesystem::path log_path{
        temporary_directory.path() / "onedrive-cpp.log"
    };
    FakeRuntimeFactory runtime_factory;

    CliFixture() {
        std::filesystem::permissions(
            temporary_directory.path(), std::filesystem::perms::owner_all
        );
        std::ofstream config{config_path};
        config << "config_version = 2\n"
               << "[console]\n"
               << "color = \"always\"\n"
               << "[logging]\n"
               << "level = \"debug\"\n"
               << "file = \"" << log_path.filename().string() << "\"\n"
               << "[sync]\n"
               << "data_directory = \"" << sync_path.string() << "\"\n"
               << "upload = false\n"
               << "[state]\n"
               << "directory = \"" << state_path.string() << "\"\n"
               << "[auth]\n"
               << "application_id = \"test-application\"\n"
               << "scopes = [\"User.Read\", \"Files.ReadWrite.All\", "
                  "\"Sites.Read.All\", \"offline_access\"]\n"
               << "[graph.throttle]\n"
               << "maximum_retries = 6\n"
               << "initial_delay_seconds = 3\n"
               << "maximum_delay_seconds = 120\n";
        config.close();
        std::filesystem::create_directories(state_path);
        std::filesystem::permissions(
            state_path, std::filesystem::perms::owner_all
        );
    }

    [[nodiscard]] RunResult authenticate() {
        return run_application(
            runtime_factory,
            {
                "onedrive-cpp",
                "account",
                "login",
                "--config",
                config_path.string(),
            }
        );
    }
};

using onedrive::test::fail;

} // namespace onedrive::test::app
