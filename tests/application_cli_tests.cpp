#include "onedrive/app/application.hpp"
#include "onedrive/app/runtime_factory.hpp"
#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/http/http_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "onedrive/storage/item_store.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
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

class FakeTokenStore final : public onedrive::auth::TokenStore {
public:
    explicit FakeTokenStore(std::filesystem::path path) : path_{std::move(path)} {}

    [[nodiscard]] std::optional<std::string> load_refresh_token() const override {
        return std::nullopt;
    }

    void save_refresh_token(const std::string&) const override {}

    [[nodiscard]] bool remove_refresh_token() const override {
        return false;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept override {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class FakeGraphClient final : public onedrive::graph::GraphClient {
public:
    [[nodiscard]] std::vector<onedrive::graph::RemoteItem> list_root() const override {
        return {};
    }
};

class FakeItemStore final : public onedrive::storage::ItemStore {
public:
    explicit FakeItemStore(int& open_count) : open_count_{open_count} {}

    void open() override {
        ++open_count_;
    }

    void upsert(onedrive::storage::ItemState) override {}

    [[nodiscard]] const onedrive::storage::ItemState* find(
        const std::string&
    ) const override {
        return nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept override {
        return 0;
    }

private:
    int& open_count_;
};

class FakeMonitor final : public onedrive::monitor::FileMonitor {
public:
    explicit FakeMonitor(int& run_count) : run_count_{run_count} {}

    [[nodiscard]] int run() const override {
        ++run_count_;
        return 0;
    }

private:
    int& run_count_;
};

class FakeMetrics final : public onedrive::metrics::Metrics {
public:
    void record_sync_run(
        bool,
        std::chrono::duration<double>
    ) noexcept override {}
};

class FakeRuntimeFactory final : public onedrive::app::RuntimeFactory {
public:
    [[nodiscard]] std::unique_ptr<onedrive::http::HttpTransport>
    create_http_transport() const override {
        throw std::logic_error{"authentication transport was not expected"};
    }

    [[nodiscard]] std::unique_ptr<onedrive::auth::DeviceAuthClient>
    create_device_auth_client(
        const onedrive::config::Config&,
        const onedrive::http::HttpTransport&
    ) const override {
        throw std::logic_error{"authentication client was not expected"};
    }

    [[nodiscard]] std::unique_ptr<onedrive::auth::TokenStore> create_token_store(
        const onedrive::config::Config& config
    ) const override {
        ++token_store_count;
        return std::make_unique<FakeTokenStore>(
            config.state_directory / "refresh_token"
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::graph::GraphClient>
    create_graph_client(const onedrive::config::Config&) const override {
        ++graph_client_count;
        return std::make_unique<FakeGraphClient>();
    }

    [[nodiscard]] std::unique_ptr<onedrive::storage::ItemStore> create_item_store(
        const onedrive::config::Config&
    ) const override {
        ++item_store_count;
        return std::make_unique<FakeItemStore>(item_store_open_count);
    }

    [[nodiscard]] std::unique_ptr<onedrive::monitor::FileMonitor> create_monitor(
        const onedrive::config::Config&
    ) const override {
        ++monitor_count;
        return std::make_unique<FakeMonitor>(monitor_run_count);
    }

    [[nodiscard]] std::unique_ptr<onedrive::metrics::Metrics>
    create_metrics() const override {
        ++metrics_count;
        return std::make_unique<FakeMetrics>();
    }

    mutable int token_store_count{0};
    mutable int graph_client_count{0};
    mutable int item_store_count{0};
    mutable int item_store_open_count{0};
    mutable int monitor_count{0};
    mutable int monitor_run_count{0};
    mutable int metrics_count{0};
};

RunResult run_application(
    FakeRuntimeFactory& runtime_factory,
    std::vector<std::string> arguments
) {
    std::vector<char*> argument_pointers;
    argument_pointers.reserve(arguments.size());
    for (auto& argument : arguments) {
        argument_pointers.push_back(argument.data());
    }

    std::ostringstream standard_output;
    std::ostringstream standard_error;
    auto* original_output = std::cout.rdbuf(standard_output.rdbuf());
    auto* original_error = std::cerr.rdbuf(standard_error.rdbuf());

    onedrive::app::Application application{runtime_factory};
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
    FakeRuntimeFactory runtime_factory;

    const auto help =
        run_application(runtime_factory, {"build/release/onedrive-cpp"});
    if (help.exit_code != 0 || !help.standard_output.contains("auth") ||
        !help.standard_output.contains("sync") ||
        !help.standard_output.contains("onedrive-cpp [OPTIONS] SUBCOMMAND") ||
        help.standard_output.contains("build/release/onedrive-cpp")) {
        return fail("no-argument invocation did not show command help");
    }

    const auto version =
        run_application(runtime_factory, {"onedrive-cpp", "--version"});
    if (version.exit_code != 0 ||
        !version.standard_output.starts_with("onedrive-cpp ")) {
        return fail("--version did not print the application version");
    }

    if (run_application(runtime_factory, {"onedrive-cpp", "unknown"}).exit_code != 2) {
        return fail("unknown command did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "auth", "--config"}
        ).exit_code != 2) {
        return fail("missing option value did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "auth", "--dry-run"}
        ).exit_code != 2) {
        return fail("command-specific option was accepted by the wrong command");
    }
    if (run_application(
            runtime_factory,
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
               << "state_directory=" << state_path.string() << '\n'
               << "graph_maximum_throttle_retries=6\n"
               << "graph_initial_throttle_delay_seconds=3\n"
               << "graph_maximum_throttle_delay_seconds=120\n";
    }

    const auto logout = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "logout",
            "--config",
            config_path.string(),
            "--log-level",
            "debug",
            "--log-file",
            log_path.string(),
        }
    );
    if (logout.exit_code != 0 ||
        !logout.standard_output.contains("No saved authentication") ||
        runtime_factory.token_store_count != 1) {
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
        runtime_factory,
        {
            "onedrive-cpp",
            "sync",
            "--config",
            config_path.string(),
            "--dry-run",
            "--log-file",
            log_path.string(),
        }
    );
    if (dry_run.exit_code != 0 ||
        !dry_run.standard_output.contains("Dry run configuration") ||
        !dry_run.standard_output.contains("throttle retries: 6") ||
        !dry_run.standard_output.contains("throttle delay:   3-120 seconds") ||
        runtime_factory.item_store_count != 1 ||
        runtime_factory.item_store_open_count != 1 ||
        runtime_factory.graph_client_count != 1 ||
        runtime_factory.metrics_count != 1) {
        return fail("sync dry-run command was not parsed or executed");
    }
    {
        std::ifstream log{log_path};
        const std::string contents{
            std::istreambuf_iterator<char>{log},
            std::istreambuf_iterator<char>{}
        };
        if (!contents.contains("Synchronization dry run completed")) {
            return fail("sync completion was not written to the configured log");
        }
    }

    const auto monitor = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "monitor",
            "--config",
            config_path.string(),
        }
    );
    if (monitor.exit_code != 0 || runtime_factory.monitor_count != 1 ||
        runtime_factory.monitor_run_count != 1) {
        return fail("monitor command was not dispatched through the runtime factory");
    }

    return EXIT_SUCCESS;
}
