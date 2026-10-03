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

    [[nodiscard]] onedrive::graph::DeltaResult list_delta(
        const std::optional<std::string>&
    ) const override {
        return {
            .changes = {
                {
                    .id = "file-id",
                    .name = "notes.txt",
                    .etag = "file-etag",
                    .parent_id = "root-id",
                    .remote_path = "notes.txt",
                    .last_modified = "2026-10-02T00:00:00Z",
                    .size = 42,
                    .directory = false,
                },
            },
            .delta_link = "https://graph.example.test/delta-token",
        };
    }

    void download_file(
        const std::string&,
        const std::filesystem::path& destination,
        const onedrive::graph::DownloadProgress&
    ) const override {
        std::ofstream output{destination, std::ios::binary};
        output << std::string(42, 'x');
    }
};

class FakeItemStore final : public onedrive::storage::ItemStore {
public:
    FakeItemStore(
        int& open_count,
        int& apply_delta_count,
        int& reset_count,
        std::string& reset_drive_id,
        int& clear_count,
        std::string& clear_drive_id
    )
        : open_count_{open_count},
          apply_delta_count_{apply_delta_count},
          reset_count_{reset_count},
          reset_drive_id_{reset_drive_id},
          clear_count_{clear_count},
          clear_drive_id_{clear_drive_id} {}

    void open() override {
        ++open_count_;
    }

    void upsert(onedrive::storage::ItemState) override {}

    void apply_delta(onedrive::storage::ItemDelta) override {
        ++apply_delta_count_;
    }

    void save_pending_download(
        onedrive::storage::PendingDownload
    ) override {}

    void remove_pending_download(
        const std::string&,
        const std::string&
    ) override {}

    [[nodiscard]] std::vector<onedrive::storage::PendingDownload>
    pending_downloads(const std::string&) const override {
        return {};
    }

    bool reset(const std::string& drive_id) override {
        ++reset_count_;
        reset_drive_id_ = drive_id;
        return true;
    }

    onedrive::storage::ClearedState clear(
        const std::string& drive_id
    ) override {
        ++clear_count_;
        clear_drive_id_ = drive_id;
        return {
            .items = 7,
            .pending_downloads = 2,
            .delta_link = true,
        };
    }

    [[nodiscard]] std::optional<std::string> delta_link(
        const std::string&
    ) const override {
        return std::nullopt;
    }

    [[nodiscard]] const onedrive::storage::ItemState* find(
        const std::string&,
        const std::string&
    ) const override {
        return nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept override {
        return 0;
    }

private:
    int& open_count_;
    int& apply_delta_count_;
    int& reset_count_;
    std::string& reset_drive_id_;
    int& clear_count_;
    std::string& clear_drive_id_;
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
        return std::make_unique<FakeItemStore>(
            item_store_open_count,
            item_store_apply_delta_count,
            item_store_reset_count,
            reset_drive_id,
            clear_count_,
            clear_drive_id_
        );
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
    mutable int item_store_apply_delta_count{0};
    mutable int item_store_reset_count{0};
    mutable std::string reset_drive_id;
    mutable int clear_count_{0};
    mutable std::string clear_drive_id_;
    mutable int monitor_count{0};
    mutable int monitor_run_count{0};
    mutable int metrics_count{0};
};

RunResult run_application(
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

    onedrive::app::Application application{runtime_factory};
    const int exit_code = application.run(
        static_cast<int>(argument_pointers.size()),
        argument_pointers.data()
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
        !help.standard_output.contains("reset-state") ||
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
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "logout", "--color", "sometimes"}
        ).exit_code != 2) {
        return fail("invalid color mode did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "logout", "--output", "yaml"}
        ).exit_code != 2) {
        return fail("invalid output mode did not return usage exit code 2");
    }
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "reset-state", "--yes"}
        ).exit_code != 2) {
        return fail("--yes was accepted without --clear-all");
    }

    TemporaryDirectory temporary_directory;
    const auto config_path = temporary_directory.path() / "config.toml";
    const auto state_path = temporary_directory.path() / "state";
    const auto sync_path = temporary_directory.path() / "files";
    const auto log_path = temporary_directory.path() / "onedrive-cpp.log";
    {
        std::ofstream config{config_path};
        config << "config_version = 1\n"
               << "[sync]\n"
               << "directory = \""
               << sync_path.string()
               << "\"\n"
               << "[state]\n"
               << "directory = \""
               << state_path.string()
               << "\"\n"
               << "[auth]\n"
               << "application_id = \"test-application\"\n"
               << "[graph.throttle]\n"
               << "maximum_retries = 6\n"
               << "initial_delay_seconds = 3\n"
               << "maximum_delay_seconds = 120\n";
    }
    std::filesystem::create_directories(state_path);
    std::filesystem::permissions(
        state_path,
        std::filesystem::perms::owner_all
    );
    {
        std::ofstream token{state_path / "refresh_token"};
        token << "test-refresh-token";
    }
    std::filesystem::permissions(
        state_path / "refresh_token",
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write
    );

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

    const auto reset_state = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "reset-state",
            "--config",
            config_path.string(),
            "--log-file",
            log_path.string(),
        }
    );
    if (reset_state.exit_code != 0 ||
        !reset_state.standard_output.contains(
            "Reset synchronization cursor for drive 'me': saved cursor removed"
        ) ||
        !reset_state.standard_output.contains(
            "Item snapshots and pending downloads were preserved"
        ) ||
        !reset_state.standard_output.contains(
            "next sync will perform a full Microsoft Graph delta query"
        ) ||
        runtime_factory.item_store_count != 1 ||
        runtime_factory.item_store_open_count != 1 ||
        runtime_factory.item_store_reset_count != 1 ||
        runtime_factory.reset_drive_id != "me" ||
        runtime_factory.graph_client_count != 0 ||
        runtime_factory.metrics_count != 0) {
        return fail("reset-state command was not dispatched to the item store");
    }
    {
        std::ifstream log{log_path};
        const std::string contents{
            std::istreambuf_iterator<char>{log},
            std::istreambuf_iterator<char>{}
        };
        if (!contents.contains(
                "Synchronization cursor reset completed for drive 'me': saved "
                "cursor removed; item snapshots and pending downloads preserved; "
                "next sync will use an initial delta query"
            )) {
            return fail("reset-state completion was not written to the log");
        }
    }

    const auto cancelled_clear = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "reset-state",
            "--clear-all",
            "--config",
            config_path.string(),
        },
        "wrong-drive\n"
    );
    if (cancelled_clear.exit_code != 1 ||
        !cancelled_clear.standard_output.contains("WARNING:") ||
        !cancelled_clear.standard_output.contains("Full state clear cancelled") ||
        runtime_factory.clear_count_ != 0 ||
        runtime_factory.item_store_count != 1) {
        return fail("full state clear accepted an invalid confirmation");
    }

    const auto confirmed_clear = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "reset-state",
            "--clear-all",
            "--config",
            config_path.string(),
            "--log-file",
            log_path.string(),
        },
        "me\n"
    );
    if (confirmed_clear.exit_code != 0 ||
        !confirmed_clear.standard_output.contains(
            "7 item snapshots and 2 pending downloads removed"
        ) ||
        !confirmed_clear.standard_output.contains(
            "Local files were not deleted"
        ) ||
        runtime_factory.clear_count_ != 1 ||
        runtime_factory.clear_drive_id_ != "me" ||
        runtime_factory.item_store_count != 2 ||
        runtime_factory.item_store_open_count != 2) {
        return fail("confirmed full state clear was not executed");
    }

    const auto automated_clear = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "reset-state",
            "--clear-all",
            "--yes",
            "--config",
            config_path.string(),
        }
    );
    if (automated_clear.exit_code != 0 ||
        automated_clear.standard_output.contains("Type the drive ID") ||
        runtime_factory.clear_count_ != 2 ||
        runtime_factory.item_store_count != 3 ||
        runtime_factory.item_store_open_count != 3) {
        return fail("--yes did not explicitly confirm full state clear");
    }

    const auto dry_run = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "sync",
            "--config",
            config_path.string(),
            "--dry-run",
            "--log-level",
            "debug",
            "--log-file",
            log_path.string(),
        }
    );
    if (dry_run.exit_code != 0 ||
        !dry_run.standard_output.contains("Dry run configuration") ||
        !dry_run.standard_output.contains(
            "Remote delta contains 1 changes (1 upserts, 0 removals)"
        ) ||
        !dry_run.standard_output.contains("throttle retries: 6") ||
        !dry_run.standard_output.contains("throttle delay:   3-120 seconds") ||
        runtime_factory.item_store_count != 4 ||
        runtime_factory.item_store_open_count != 4 ||
        runtime_factory.item_store_apply_delta_count != 0 ||
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
        if (!contents.contains(
                "Preparing Microsoft Graph delta query for drive 'me': 0 tracked "
                "items, saved cursor absent"
            ) ||
            !contents.contains(
                "Remote delta prepared for drive 'me': 1 upserts, 0 removals"
            ) ||
            !contents.contains(
                "Synchronization plan for drive 'me': 0 directories, 1 "
                "downloads, 42 bytes, 0 deferred local removals"
            ) ||
            !contents.contains(
                "Dry run left synchronization state unchanged for drive 'me'"
            ) ||
            !contents.contains("Synchronization dry run completed")) {
            return fail("sync dry-run diagnostics were not written to the log");
        }
    }

    const auto trace_sync = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "sync",
            "--config",
            config_path.string(),
            "--log-level",
            "trace",
            "--log-file",
            log_path.string(),
        }
    );
    if (trace_sync.exit_code != 0 ||
        !trace_sync.standard_output.contains(
            "Remote delta contains 1 changes (1 upserts, 0 removals)"
        ) ||
        runtime_factory.item_store_apply_delta_count != 1) {
        return fail("sync trace command did not apply the remote delta");
    }
    {
        std::ifstream log{log_path};
        const std::string contents{
            std::istreambuf_iterator<char>{log},
            std::istreambuf_iterator<char>{}
        };
        if (!contents.contains(
                "Remote delta prepared for drive 'me': 1 upserts, 0 removals"
            ) ||
            !contents.contains("Persisting remote delta for drive 'me'") ||
            !contents.contains("Downloading 'notes.txt' (42 bytes)") ||
            !contents.contains(
                "Atomically installed 'notes.txt' (42 bytes)"
            ) ||
            !contents.contains(
                "Download execution completed: 1 downloaded, 0 reused, 0 "
                "directories prepared"
            ) ||
            !contents.contains(
                "Remote item changed: path='notes.txt', id='file-id', "
                "eTag='file-etag', type=file"
            )) {
            return fail("remote delta item metadata was not written at trace level");
        }
    }

    const auto json_dry_run = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "sync",
            "--config",
            config_path.string(),
            "--dry-run",
            "--output",
            "json",
            "--color",
            "always",
        }
    );
    if (json_dry_run.exit_code != 0 ||
        !json_dry_run.standard_output.contains(
            "\"event\":\"dry_run_configuration\""
        ) ||
        !json_dry_run.standard_output.contains(
            "\"event\":\"synchronization_plan\""
        ) ||
        !json_dry_run.standard_output.contains(
            "\"event\":\"sync_completed\""
        ) ||
        json_dry_run.standard_output.contains("\033[")) {
        return fail("JSON output was not emitted as unstyled JSON Lines");
    }

    const auto quiet_dry_run = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "sync",
            "--config",
            config_path.string(),
            "--dry-run",
            "--quiet",
        }
    );
    if (quiet_dry_run.exit_code != 0 ||
        !quiet_dry_run.standard_output.empty()) {
        return fail("quiet output did not suppress sync information");
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
