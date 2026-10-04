#include "onedrive/app/application.hpp"
#include "onedrive/app/runtime_factory.hpp"
#include "onedrive/app/runtime_options.hpp"
#include "onedrive/account/account_state.hpp"
#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/http/http_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "onedrive/storage/item_store.hpp"
#include "test_support.hpp"

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

using onedrive::test::TemporaryDirectory;

struct RunResult {
    int exit_code;
    std::string standard_output;
    std::string standard_error;
};

class FakeTokenStore final {
public:
    explicit FakeTokenStore(std::filesystem::path path) : path_{std::move(path)} {}

    [[nodiscard]] std::optional<std::string> load_refresh_token() const {
        std::ifstream input{path_};
        if (!input) {
            return std::nullopt;
        }
        return std::string{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}
        };
    }

    void save_refresh_token(const std::string& token) const {
        std::ofstream output{path_};
        output << token;
    }

    [[nodiscard]] bool remove_refresh_token() const {
        return std::filesystem::remove(path_);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class FakeAuthenticationTransport final {
public:
    [[nodiscard]] onedrive::http::HttpResult perform(
        const onedrive::http::HttpRequest& request
    ) const {
        if (request.url.ends_with("/oauth2/v2.0/devicecode")) {
            return onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"device_code":"device-code","user_code":"ABCD-EFGH",)"
                    R"("verification_uri":"https://microsoft.com/link",)"
                    R"("message":"Authenticate the test account",)"
                    R"("expires_in":900,"interval":1})",
            };
        }
        if (request.url.ends_with("/oauth2/v2.0/token")) {
            return onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"access-token",)"
                    R"("refresh_token":"refresh-token"})",
            };
        }
        if (request.url.ends_with("/me?$select=id,displayName")) {
            return onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"id":"user-id","displayName":"Test User"})",
            };
        }
        if (request.url.ends_with("/me/drive?$select=id,name")) {
            return onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"id":"drive-id","name":"Test Drive"})",
            };
        }
        if (request.url.ends_with("/me/photo/$value")) {
            return onedrive::http::HttpResponse{
                .status_code = 200,
                .headers = {
                    {.name = "Content-Type", .value = "image/jpeg"},
                },
                .body = "avatar",
            };
        }
        return std::unexpected(
            onedrive::http::HttpError{
                .message = "unexpected authentication request: " + request.url,
            }
        );
    }

    [[nodiscard]] onedrive::http::HttpResult download(
        const onedrive::http::HttpRequest&,
        const std::filesystem::path&,
        const onedrive::http::DownloadProgress&,
        const onedrive::http::DownloadData&,
        const onedrive::http::DownloadCheckpoint&,
        const onedrive::http::DownloadResponseGate&
    ) const {
        return std::unexpected(
            onedrive::http::HttpError{
                .message = "authentication download was not expected",
            }
        );
    }
};

class FakeGraphClient final {
public:
    explicit FakeGraphClient(std::string configured_drive_id)
        : configured_drive_id_{std::move(configured_drive_id)} {}

    [[nodiscard]] onedrive::account::DriveIdentity drive_identity()
        const {
        return {
            .user_id = "user-id",
            .user_display_name = "Test User",
            .configured_drive_id = configured_drive_id_,
            .drive_id = "drive-id",
            .drive_name = "Test Drive",
        };
    }

    [[nodiscard]] std::vector<onedrive::graph::RemoteItem> list_root() const {
        return {};
    }

    [[nodiscard]] onedrive::graph::RemoteItem item_by_path(
        const std::string& remote_path
    ) const {
        return {
            .id = "single-file-id",
            .name = std::filesystem::path{remote_path}.filename().string(),
            .etag = "single-file-etag",
            .parent_id = "root-id",
            .remote_path = remote_path,
            .last_modified = "2026-10-02T00:00:00Z",
            .size = 42,
            .directory = false,
        };
    }

    [[nodiscard]] onedrive::graph::DeltaResult list_delta(
        const std::optional<std::string>&,
        const onedrive::graph::DeltaProgress& progress
    ) const {
        if (progress) {
            progress(1, 1, true);
        }
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
        const std::string&,
        std::uint64_t,
        const std::filesystem::path& destination,
        std::uint64_t,
        std::stop_token,
        const onedrive::graph::DownloadProgress&,
        const onedrive::graph::DownloadCheckpoint& checkpoint,
        const onedrive::graph::DownloadData& data
    ) const {
        const std::string contents(42, 'x');
        std::ofstream output{destination, std::ios::binary};
        output << contents;
        if (data) {
            data(0, std::as_bytes(std::span{contents}));
        }
        if (checkpoint) {
            checkpoint(42);
        }
    }

    [[nodiscard]] onedrive::graph::RemoteItem upload_file(
        const std::string&,
        const std::optional<std::string>&,
        const std::string&,
        const std::filesystem::path&
    ) const {
        throw std::logic_error{"upload was not expected"};
    }

private:
    std::string configured_drive_id_;
};

class FakeItemStore final {
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

    void open() {
        ++open_count_;
    }

    void upsert(onedrive::storage::ItemState) {}

    void apply_delta(onedrive::storage::ItemDelta) {
        ++apply_delta_count_;
    }

    void save_pending_download(
        onedrive::storage::PendingDownload
    ) {}

    void remove_pending_download(
        const std::string&,
        const std::string&
    ) {}

    [[nodiscard]] std::vector<onedrive::storage::PendingDownload>
    pending_downloads(const std::string&) const {
        return {};
    }

    void save_partial_download(onedrive::storage::PartialDownload) {}

    void remove_partial_download(
        const std::string&,
        const std::string&
    ) {}

    [[nodiscard]] std::optional<onedrive::storage::PartialDownload>
    partial_download(
        const std::string&,
        const std::string&
    ) const {
        return std::nullopt;
    }

    void save_pending_upload(onedrive::storage::PendingUpload) {}

    [[nodiscard]] std::vector<onedrive::storage::PendingUpload>
    pending_uploads(const std::string&) const {
        return {};
    }

    void commit_upload(
        const onedrive::storage::PendingUpload&,
        onedrive::storage::ItemState
    ) {}

    [[nodiscard]] std::vector<onedrive::storage::BlockedItem> blocked_items(
        const std::string&
    ) const {
        return {};
    }

    bool reset(const std::string& drive_id) {
        ++reset_count_;
        reset_drive_id_ = drive_id;
        return true;
    }

    onedrive::storage::ClearedState clear(
        const std::string& drive_id
    ) {
        ++clear_count_;
        clear_drive_id_ = drive_id;
        return {
            .items = 7,
            .pending_downloads = 2,
            .partial_downloads = 4,
            .pending_uploads = 1,
            .blocked_items = 3,
            .delta_link = true,
        };
    }

    [[nodiscard]] std::optional<std::string> delta_link(
        const std::string&
    ) const {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> sync_filter_fingerprint(
        const std::string&
    ) const {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<onedrive::storage::ItemState> find(
        const std::string&,
        const std::string&
    ) const {
        return std::nullopt;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return 0;
    }

    [[nodiscard]] std::vector<onedrive::storage::ItemState> drive_items(
        const std::string&
    ) const {
        return {};
    }

private:
    int& open_count_;
    int& apply_delta_count_;
    int& reset_count_;
    std::string& reset_drive_id_;
    int& clear_count_;
    std::string& clear_drive_id_;
};

class FakeMonitor final {
public:
    explicit FakeMonitor(int& run_count) : run_count_{run_count} {}

    [[nodiscard]] int run() const {
        ++run_count_;
        return 0;
    }

private:
    int& run_count_;
};

class FakeMetrics final {
public:
    void record_sync_run(
        bool,
        std::chrono::duration<double>
    ) noexcept {}
};

class FakeRuntimeFactory final {
public:
    [[nodiscard]] std::unique_ptr<onedrive::http::HttpTransport>
    create_http_transport(const onedrive::config::Config&) const {
        return std::make_unique<onedrive::http::HttpTransport>(
            std::in_place_type<FakeAuthenticationTransport>
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::auth::DeviceAuthClient>
    create_device_auth_client(
        const onedrive::config::Config& config,
        const onedrive::http::HttpTransport& transport
    ) const {
        return std::make_unique<onedrive::auth::DeviceAuthClient>(
            &transport,
            onedrive::app::device_auth_options(config),
            [](std::chrono::seconds) {}
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::auth::TokenStore> create_token_store(
        const onedrive::config::Config& config
    ) const {
        ++token_store_count;
        return std::make_unique<onedrive::auth::TokenStore>(
            std::in_place_type<FakeTokenStore>,
            onedrive::account::AccountState::active_token_directory(
                config.state_directory
            ) / "refresh_token"
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::graph::GraphClient>
    create_graph_client(const onedrive::config::Config&) const {
        ++graph_client_count;
        return std::make_unique<onedrive::graph::GraphClient>(
            std::in_place_type<FakeGraphClient>,
            configured_drive_id
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::storage::ItemStore> create_item_store(
        const onedrive::config::Config& config,
        const onedrive::account::DriveIdentity&
    ) const {
        ++item_store_count;
        last_item_store_sync_directory = config.sync_directory;
        return std::make_unique<onedrive::storage::ItemStore>(
            std::in_place_type<FakeItemStore>,
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
    ) const {
        ++monitor_count;
        return std::make_unique<onedrive::monitor::FileMonitor>(
            std::in_place_type<FakeMonitor>,
            monitor_run_count
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::metrics::Metrics>
    create_metrics() const {
        ++metrics_count;
        return std::make_unique<onedrive::metrics::Metrics>(
            std::in_place_type<FakeMetrics>
        );
    }

    std::string configured_drive_id{"me"};
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
    mutable std::filesystem::path last_item_store_sync_directory;
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

    onedrive::app::RuntimeFactory runtime_factory_proxy{
        onedrive::detail::borrowed_proxy,
        runtime_factory
    };
    onedrive::app::Application application{&runtime_factory_proxy};
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

using onedrive::test::fail;

}  // namespace

int main() {
    FakeRuntimeFactory runtime_factory;

    const auto help =
        run_application(runtime_factory, {"build/release/onedrive-cpp"});
    if (help.exit_code != 0 || !help.standard_output.contains("auth") ||
        !help.standard_output.contains("reset-state") ||
        !help.standard_output.contains("download") ||
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
    if (run_application(
            runtime_factory,
            {"onedrive-cpp", "download"}
        ).exit_code != 2) {
        return fail("download command accepted a missing remote path");
    }

    TemporaryDirectory temporary_directory;
    const auto config_path = temporary_directory.path() / "config.toml";
    const auto state_path = temporary_directory.path() / "state";
    const auto sync_path = temporary_directory.path() / "files";
    const auto log_path = temporary_directory.path() / "onedrive-cpp.log";
    {
        std::ofstream config{config_path};
        config << "config_version = 2\n"
               << "[sync]\n"
               << "directory = \""
               << sync_path.string()
               << "\"\n"
               << "upload = false\n"
               << "[state]\n"
               << "directory = \""
               << state_path.string()
               << "\"\n"
               << "[auth]\n"
               << "application_id = \"test-application\"\n"
               << "scopes = [\"User.Read\", \"Files.ReadWrite.All\", "
                  "\"offline_access\"]\n"
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
    const auto authentication = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "auth",
            "--config",
            config_path.string(),
        }
    );
    const auto account_path =
        onedrive::account::AccountState::active_token_directory(state_path);
    if (authentication.exit_code != 0 ||
        !authentication.standard_output.contains(
            "Authentication succeeded"
        ) ||
        !authentication.standard_output.contains(
            "WARNING: Authentication requests broad organizational"
        ) ||
        !account_path.filename().string().starts_with("Test-User--") ||
        !std::filesystem::exists(account_path / "avatar.jpg") ||
        !std::filesystem::exists(account_path / "account.json") ||
        !std::filesystem::exists(account_path / "refresh_token") ||
        std::filesystem::directory_iterator{
            account_path / "drives"
        } == std::filesystem::directory_iterator{}) {
        return fail("authentication did not initialize account state");
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
        !logout.standard_output.contains("Saved authentication removed") ||
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
    if (run_application(
            runtime_factory,
            {
                "onedrive-cpp",
                "auth",
                "--config",
                config_path.string(),
            }
        ).exit_code != 0) {
        return fail("reauthentication did not restore account state");
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
            "Reset synchronization cursor for drive 'me' (drive-id): saved "
            "cursor removed"
        ) ||
        !reset_state.standard_output.contains(
            "Item snapshots, pending downloads, partial downloads, pending "
            "uploads, and blocked items were preserved"
        ) ||
        !reset_state.standard_output.contains(
            "next sync will perform a full Microsoft Graph delta query"
        ) ||
        runtime_factory.item_store_count != 1 ||
        runtime_factory.item_store_open_count != 1 ||
        runtime_factory.item_store_reset_count != 1 ||
        runtime_factory.reset_drive_id != "drive-id" ||
        runtime_factory.graph_client_count != 1 ||
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
                "Synchronization cursor reset completed for drive 'drive-id': saved "
                "cursor removed; item snapshots, pending downloads, partial "
                "downloads, pending uploads, and blocked items preserved; next "
                "sync will use an initial delta query"
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
        !confirmed_clear.standard_output.contains("drive 'me' (drive-id)") ||
        !confirmed_clear.standard_output.contains(
            "Type the configured drive reference 'me'"
        ) ||
        !confirmed_clear.standard_output.contains(
            "7 item snapshots, 2 pending downloads, 4 partial downloads, 1 "
            "pending uploads, and 3 blocked items removed"
        ) ||
        !confirmed_clear.standard_output.contains(
            "Cleared all synchronization state for drive 'me' (drive-id)"
        ) ||
        !confirmed_clear.standard_output.contains(
            "Local files were not deleted"
        ) ||
        runtime_factory.clear_count_ != 1 ||
        runtime_factory.clear_drive_id_ != "drive-id" ||
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
        automated_clear.standard_output.contains(
            "Type the configured drive reference"
        ) ||
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
    const auto expected_sync_directory =
        onedrive::account::AccountState::drive_data_directory(
            sync_path,
            {
                .user_id = "user-id",
                .user_display_name = "Test User",
                .configured_drive_id = "me",
                .drive_id = "drive-id",
                .drive_name = "Test Drive",
            }
        );
    if (dry_run.exit_code != 0 ||
        !dry_run.standard_output.contains("Dry run configuration") ||
        !dry_run.standard_output.contains(
            "Remote delta contains 1 changes (1 upserts, 0 removals, 0 moves, "
            "0 blocked)"
        ) ||
        !dry_run.standard_output.contains("throttle retries:     6") ||
        !dry_run.standard_output.contains(
            "throttle delay:       3-120 seconds"
        ) ||
        !dry_run.standard_output.contains("download concurrency: 4") ||
        !dry_run.standard_output.contains("per-download rate:    0") ||
        !dry_run.standard_output.contains("total download rate:  0") ||
        runtime_factory.item_store_count != 4 ||
        runtime_factory.item_store_open_count != 4 ||
        runtime_factory.item_store_apply_delta_count != 0 ||
        runtime_factory.graph_client_count != 5 ||
        runtime_factory.metrics_count != 1 ||
        runtime_factory.last_item_store_sync_directory !=
            expected_sync_directory ||
        !dry_run.standard_output.contains(expected_sync_directory.string())) {
        return fail("sync dry-run command was not parsed or executed");
    }
    {
        std::ifstream log{log_path};
        const std::string contents{
            std::istreambuf_iterator<char>{log},
            std::istreambuf_iterator<char>{}
        };
        if (!contents.contains(
                "Preparing Microsoft Graph delta query for drive 'drive-id': 0 tracked "
                "items, saved cursor absent"
            ) ||
            !contents.contains(
                "Remote delta prepared for drive 'drive-id': 1 upserts, 0 removals"
            ) ||
            !contents.contains(
                "Synchronization plan for drive 'drive-id': 0 directories, 1 "
                "downloads, 42 bytes, 0 deferred local removals"
            ) ||
            !contents.contains(
                "Dry run left synchronization state unchanged for drive 'drive-id'"
            ) ||
            !contents.contains("Synchronization dry run completed")) {
            return fail("sync dry-run diagnostics were not written to the log");
        }
    }

    const std::string single_remote_path{"Documents/single file.txt"};
    const auto single_destination =
        expected_sync_directory / single_remote_path;
    const int stores_before_single_download =
        runtime_factory.item_store_count;
    const auto single_dry_run = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "download",
            single_remote_path,
            "--config",
            config_path.string(),
            "--dry-run",
        }
    );
    if (single_dry_run.exit_code != 0 ||
        !single_dry_run.standard_output.contains(
            "Single-file download plan"
        ) ||
        std::filesystem::exists(single_destination) ||
        runtime_factory.item_store_count !=
            stores_before_single_download ||
        runtime_factory.item_store_apply_delta_count != 0) {
        return fail("single-file download dry run changed local state");
    }

    const auto single_download = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "download",
            single_remote_path,
            "--config",
            config_path.string(),
        }
    );
    if (single_download.exit_code != 0 ||
        !single_download.standard_output.contains(
            "Downloaded 'Documents/single file.txt'"
        ) ||
        !std::filesystem::is_regular_file(single_destination) ||
        std::filesystem::file_size(single_destination) != 42 ||
        runtime_factory.item_store_count !=
            stores_before_single_download + 1 ||
        runtime_factory.item_store_apply_delta_count != 0) {
        return fail("single-file download was not safely dispatched");
    }

    const auto legacy_file = sync_path / "legacy.txt";
    std::filesystem::create_directories(sync_path);
    {
        std::ofstream output{legacy_file};
        output << "legacy";
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
            "Remote delta contains 1 changes (1 upserts, 0 removals, 0 moves, "
            "0 blocked)"
        ) ||
        runtime_factory.item_store_apply_delta_count != 1) {
        return fail("sync trace command did not apply the remote delta");
    }
    if (!std::filesystem::is_regular_file(
            expected_sync_directory / "notes.txt"
        )) {
        return fail("synchronized file was not isolated by account and Drive");
    }
    if (std::filesystem::exists(sync_path / "notes.txt")) {
        return fail("synchronized file was written to the flat data root");
    }
    if (!std::filesystem::is_regular_file(legacy_file)) {
        return fail("legacy flat-layout file was modified by synchronization");
    }
    {
        std::ifstream log{log_path};
        const std::string contents{
            std::istreambuf_iterator<char>{log},
            std::istreambuf_iterator<char>{}
        };
        if (!contents.contains(
                "Remote delta prepared for drive 'drive-id': 1 upserts, 0 removals"
            ) ||
            !contents.contains("Persisting remote delta for drive 'drive-id'") ||
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
