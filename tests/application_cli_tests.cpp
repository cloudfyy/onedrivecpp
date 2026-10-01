#include "onedrive/app/application.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_{
              std::filesystem::temp_directory_path() /
              ("onedrive-cpp-cli-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
          } {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

struct RunResult {
    int exit_code;
    std::string standard_output;
    std::string standard_error;
};

RunResult run_application(std::vector<std::string> arguments) {
    std::vector<char*> argument_pointers;
    argument_pointers.reserve(arguments.size());
    for (auto& argument : arguments) {
        argument_pointers.push_back(argument.data());
    }

    std::ostringstream standard_output;
    std::ostringstream standard_error;
    auto* original_output = std::cout.rdbuf(standard_output.rdbuf());
    auto* original_error = std::cerr.rdbuf(standard_error.rdbuf());

    onedrive::app::Application application;
    const int exit_code = application.run(
        static_cast<int>(argument_pointers.size()),
        argument_pointers.data()
    );

    std::cout.rdbuf(original_output);
    std::cerr.rdbuf(original_error);
    return {
        .exit_code = exit_code,
        .standard_output = std::move(standard_output).str(),
        .standard_error = std::move(standard_error).str(),
    };
}

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    const auto help = run_application({"build/release/onedrive-cpp"});
    if (help.exit_code != 0 || !help.standard_output.contains("auth") ||
        !help.standard_output.contains("sync") ||
        !help.standard_output.contains("onedrive-cpp [OPTIONS] SUBCOMMAND") ||
        help.standard_output.contains("build/release/onedrive-cpp")) {
        return fail("no-argument invocation did not show command help");
    }

    const auto version = run_application({"onedrive-cpp", "--version"});
    if (version.exit_code != 0 ||
        !version.standard_output.starts_with("onedrive-cpp ")) {
        return fail("--version did not print the application version");
    }

    if (run_application({"onedrive-cpp", "unknown"}).exit_code != 2) {
        return fail("unknown command did not return usage exit code 2");
    }
    if (run_application({"onedrive-cpp", "auth", "--config"}).exit_code != 2) {
        return fail("missing option value did not return usage exit code 2");
    }
    if (run_application({"onedrive-cpp", "auth", "--dry-run"}).exit_code != 2) {
        return fail("command-specific option was accepted by the wrong command");
    }
    if (run_application(
            {"onedrive-cpp", "logout", "--log-level", "verbose"}
        ).exit_code != 2) {
        return fail("invalid log level did not return usage exit code 2");
    }

    TemporaryDirectory temporary_directory;
    const auto config_path = temporary_directory.path() / "config";
    const auto state_path = temporary_directory.path() / "state";
    const auto log_path = temporary_directory.path() / "onedrive-cpp.log";
    {
        std::ofstream config{config_path};
        config << "sync_directory=" << temporary_directory.path().string() << '\n'
               << "state_directory=" << state_path.string() << '\n';
    }

    const auto logout = run_application({
        "onedrive-cpp",
        "logout",
        "--config",
        config_path.string(),
        "--log-level",
        "debug",
        "--log-file",
        log_path.string(),
    });
    if (logout.exit_code != 0 ||
        !logout.standard_output.contains("No saved authentication")) {
        return fail("logout did not accept a subcommand configuration path");
    }
    {
        std::ifstream log{log_path};
        const std::string contents{
            std::istreambuf_iterator<char>{log},
            std::istreambuf_iterator<char>{}
        };
        if (!contents.contains("Removing locally saved authentication")) {
            return fail("configured log file did not receive application logs");
        }
    }

    const auto dry_run = run_application(
        {
            "onedrive-cpp",
            "sync",
            "--config",
            config_path.string(),
            "--dry-run",
        }
    );
    if (dry_run.exit_code != 0 ||
        !dry_run.standard_output.contains("Dry run configuration")) {
        return fail("sync dry-run command was not parsed or executed");
    }

    return EXIT_SUCCESS;
}
