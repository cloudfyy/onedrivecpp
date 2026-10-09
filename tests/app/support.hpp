#include "onedrive/ui/cli/app.hpp"
#include "onedrive/app/factory.hpp"
#include "onedrive/app/options.hpp"
#include "onedrive/account/account_state.hpp"
#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/config/config.hpp"
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

class FakeTokenStore final {
public:
    explicit FakeTokenStore(std::filesystem::path path)
        : path_{std::move(path)} {
    }

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
    using ResponseOverride = std::function<std::optional<
        onedrive::http::HttpResult>(const onedrive::http::HttpRequest&)>;

    explicit FakeAuthenticationTransport(
        ResponseOverride response_override = {}
    )
        : response_override_{std::move(response_override)} {
    }

    [[nodiscard]] onedrive::http::HttpResult
    perform(const onedrive::http::HttpRequest& request) const {
        if (response_override_) {
            if (auto result = response_override_(request)) {
                return std::move(*result);
            }
        }
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
                .body = R"({"token_type":"Bearer","expires_in":3600,)"
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
                .headers =
                    {
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

private:
    ResponseOverride response_override_;
};

class FakeGraphClient final {
public:
    explicit FakeGraphClient(
        std::string configured_drive_id,
        bool empty_shared = false,
        bool empty_sites = false,
        bool include_configured_drive = false
    )
        : configured_drive_id_{std::move(configured_drive_id)},
          empty_shared_{empty_shared},
          empty_sites_{empty_sites},
          include_configured_drive_{include_configured_drive} {
    }

    [[nodiscard]] onedrive::account::DriveIdentity drive_identity() const {
        return onedrive::test::test_drive_identity(configured_drive_id_);
    }

    [[nodiscard]] std::vector<onedrive::graph::DriveInfo> list_drives() const {
        std::vector<onedrive::graph::DriveInfo> drives{
            {
                .id = "shared-drive-id",
                .name = "Shared Drive",
                .type = "business",
                .web_url = "https://example.test/shared",
                .owner = "Example Team",
                .quota = std::nullopt,
            },
        };
        if (include_configured_drive_) {
            drives.push_back(drive_info());
        }
        return drives;
    }

    [[nodiscard]] onedrive::graph::DriveInfo drive_info() const {
        return {
            .id = "drive-id",
            .name = "Test Drive",
            .type = "business",
            .web_url = "https://example.test/drive",
            .owner = "Test User",
            .quota = onedrive::graph::DriveQuota{
                .total = 2'048,
                .used = 400,
                .remaining = 600,
                .deleted = 25,
                .state = "normal",
            },
        };
    }

    [[nodiscard]] std::vector<onedrive::graph::SharedResource>
    list_shared_resources() const {
        if (empty_shared_) {
            return {};
        }
        return {
            {
                .name = "Shared Plan",
                .target_name = "Plan",
                .drive_id = "team-drive-id",
                .item_id = "shared-item-id",
                .web_url = "https://example.test/shared-plan",
                .owner = "Example Team",
                .local_path = {},
                .directory = true,
                .source = onedrive::graph::SharedResourceSource::shared_with_me,
            },
            {
                .name = "Team Shortcut",
                .target_name = "Team Files",
                .drive_id = "team-drive-id",
                .item_id = "shortcut-target-id",
                .web_url = "https://example.test/team-files",
                .owner = "Example Team",
                .local_path = "Team Shortcut",
                .directory = true,
                .source = onedrive::graph::SharedResourceSource::shortcut,
            },
        };
    }

    [[nodiscard]] std::vector<onedrive::graph::SiteInfo>
    search_sites(const std::string& query) const {
        if (empty_sites_ || query != "Engineering") {
            return {};
        }
        return {
            {
                .id = "example.sharepoint.test,site-id,web-id",
                .name = "engineering",
                .display_name = "Engineering",
                .web_url = "https://example.sharepoint.test/engineering",
                .drives = {
                    {
                        .id = "library-drive-id",
                        .name = "Documents",
                        .type = "documentLibrary",
                        .web_url =
                            "https://example.sharepoint.test/engineering/docs",
                        .owner = "Engineering",
                        .quota = std::nullopt,
                    },
                },
            },
        };
    }

    [[nodiscard]] std::vector<onedrive::graph::RemoteItem> list_root() const {
        return {};
    }

    [[nodiscard]] onedrive::graph::NotificationChannel
    notification_channel() const {
        return {
            .notification_url = "https://notification.example.test/token",
            .expires_at =
                std::chrono::system_clock::now() + std::chrono::hours{1},
        };
    }

    void refresh_access_token() const {
    }

    [[nodiscard]] onedrive::graph::RemoteItem
    item_by_path(const std::string& remote_path) const {
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
    [[nodiscard]] onedrive::graph::RemoteItem item_by_path(
        const std::string& remote_path, std::stop_token stop_token
    ) const {
        if (stop_token.stop_requested()) {
            throw onedrive::graph::RequestCancelledError{
                "simulated path lookup cancellation"
            };
        }
        return item_by_path(remote_path);
    }

    [[nodiscard]] onedrive::graph::DeltaResult list_delta(
        const std::optional<std::string>&,
        const onedrive::graph::DeltaProgress& progress,
        std::stop_token
    ) const {
        if (progress) {
            progress(1, 1, onedrive::util::ProgressState::completed);
        }
        return {
            .changes =
                {
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
        const std::filesystem::path&,
        const std::optional<onedrive::graph::UploadSession>&,
        const onedrive::graph::UploadCheckpoint&,
        std::stop_token
    ) const {
        throw std::logic_error{"upload was not expected"};
    }

    [[nodiscard]] onedrive::graph::RemoteItem
    create_directory(const std::string&) const {
        throw std::logic_error{"directory creation was not expected"};
    }
    [[nodiscard]] onedrive::graph::RemoteItem create_directory(
        const std::string& remote_path, std::stop_token stop_token
    ) const {
        if (stop_token.stop_requested()) {
            throw onedrive::graph::RequestCancelledError{
                "simulated directory creation cancellation"
            };
        }
        return create_directory(remote_path);
    }

    void delete_item(const std::string&, const std::string&) const {
    }
    void delete_item(
        const std::string& remote_id,
        const std::string& expected_etag,
        std::stop_token stop_token
    ) const {
        if (stop_token.stop_requested()) {
            throw onedrive::graph::RequestCancelledError{
                "simulated item deletion cancellation"
            };
        }
        delete_item(remote_id, expected_etag);
    }
    [[nodiscard]] onedrive::graph::RemoteItem move_item(
        const std::string&, const std::string&, const std::string&
    ) const {
        return {};
    }
    [[nodiscard]] onedrive::graph::RemoteItem move_item(
        const std::string& remote_id,
        const std::string& expected_etag,
        const std::string& destination_path,
        std::stop_token stop_token
    ) const {
        if (stop_token.stop_requested()) {
            throw onedrive::graph::RequestCancelledError{
                "simulated item move cancellation"
            };
        }
        return move_item(remote_id, expected_etag, destination_path);
    }

private:
    std::string configured_drive_id_;
    bool empty_shared_;
    bool empty_sites_;
    bool include_configured_drive_;
};

class FakeItemStore final {
public:
    FakeItemStore(
        int& open_count,
        int& apply_delta_count,
        int& reset_count,
        std::string& reset_drive_id,
        int& clear_count,
        std::string& clear_drive_id,
        const std::vector<onedrive::storage::ItemState>& items,
        const std::vector<onedrive::storage::PartialDownload>& partials,
        const std::vector<onedrive::storage::PendingDownload>& pending,
        const std::vector<onedrive::storage::BlockedItem>& blocked
    )
        : open_count_{open_count},
          apply_delta_count_{apply_delta_count},
          reset_count_{reset_count},
          reset_drive_id_{reset_drive_id},
          clear_count_{clear_count},
          clear_drive_id_{clear_drive_id},
          items_{items},
          partials_{partials},
          pending_{pending},
          blocked_{blocked} {
    }

    void open() {
        ++open_count_;
    }

    void open_read_only() {
        ++open_count_;
    }

    void upsert(onedrive::storage::ItemState) {
    }

    void apply_delta(onedrive::storage::ItemDelta) {
        ++apply_delta_count_;
    }

    void save_pending_download(onedrive::storage::PendingDownload) {
    }

    void remove_pending_download(const std::string&, const std::string&) {
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingDownload>
    pending_downloads(const std::string& drive_id) const {
        auto filtered =
            pending_ | std::views::filter([&](const auto& download) {
                return download.item.drive_id == drive_id;
            });
        return {filtered.begin(), filtered.end()};
    }

    void save_partial_download(onedrive::storage::PartialDownload) {
    }

    void remove_partial_download(const std::string&, const std::string&) {
    }

    [[nodiscard]] std::optional<onedrive::storage::PartialDownload>
    partial_download(const std::string&, const std::string&) const {
        return std::nullopt;
    }

    [[nodiscard]] std::vector<onedrive::storage::PartialDownload>
    partial_downloads(const std::string& drive_id) const {
        auto filtered =
            partials_ | std::views::filter([&](const auto& partial) {
                return partial.item.drive_id == drive_id;
            });
        return {filtered.begin(), filtered.end()};
    }

    void save_pending_upload(onedrive::storage::PendingUpload) {
    }
    void remove_pending_upload(const std::string&, const std::string&) {
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingUpload>
    pending_uploads(const std::string&) const {
        return {};
    }

    void commit_upload(
        const onedrive::storage::PendingUpload&, onedrive::storage::ItemState
    ) {
    }

    void save_pending_delete(onedrive::storage::PendingDelete) {
    }
    void remove_pending_delete(const std::string&, const std::string&) {
    }
    [[nodiscard]] std::vector<onedrive::storage::PendingDelete>
    pending_deletes(const std::string&) const {
        return {};
    }
    void commit_delete(const onedrive::storage::PendingDelete&) {
    }
    void save_pending_remote_move(onedrive::storage::PendingRemoteMove) {
    }
    void remove_pending_remote_move(const std::string&, const std::string&) {
    }
    [[nodiscard]] std::vector<onedrive::storage::PendingRemoteMove>
    pending_remote_moves(const std::string&) const {
        return {};
    }
    void commit_remote_move(
        const onedrive::storage::PendingRemoteMove&,
        onedrive::storage::ItemState
    ) {
    }

    void save_pending_move(onedrive::storage::PendingMove) {
    }

    void remove_pending_move(const std::string&, const std::string&) {
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingMove>
    pending_moves(const std::string&) const {
        return {};
    }

    [[nodiscard]] std::vector<onedrive::storage::UploadSuppression>
    upload_suppressions(const std::string&) const {
        return {};
    }

    void remove_upload_suppression(
        const std::string&, const std::filesystem::path&
    ) {
    }

    [[nodiscard]] std::vector<onedrive::storage::BlockedItem>
    blocked_items(const std::string& drive_id) const {
        auto filtered = blocked_ | std::views::filter([&](const auto& item) {
                            return item.drive_id == drive_id;
                        });
        return {filtered.begin(), filtered.end()};
    }

    bool reset(const std::string& drive_id) {
        ++reset_count_;
        reset_drive_id_ = drive_id;
        return true;
    }

    onedrive::storage::ClearedState clear(const std::string& drive_id) {
        ++clear_count_;
        clear_drive_id_ = drive_id;
        return {
            .items = 7,
            .pending_downloads = 2,
            .partial_downloads = 4,
            .pending_uploads = 1,
            .pending_moves = 5,
            .upload_suppressions = 6,
            .blocked_items = 3,
            .delta_link = true,
        };
    }

    [[nodiscard]] std::optional<std::string>
    delta_link(const std::string&) const {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string>
    sync_filter_fingerprint(const std::string&) const {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<onedrive::storage::ItemState>
    find(const std::string&, const std::string&) const {
        return std::nullopt;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return 0;
    }

    [[nodiscard]] std::vector<onedrive::storage::ItemState>
    drive_items(const std::string& drive_id) const {
        auto filtered = items_ | std::views::filter([&](const auto& item) {
                            return item.drive_id == drive_id;
                        });
        return {filtered.begin(), filtered.end()};
    }

private:
    int& open_count_;
    int& apply_delta_count_;
    int& reset_count_;
    std::string& reset_drive_id_;
    int& clear_count_;
    std::string& clear_drive_id_;
    const std::vector<onedrive::storage::ItemState>& items_;
    const std::vector<onedrive::storage::PartialDownload>& partials_;
    const std::vector<onedrive::storage::PendingDownload>& pending_;
    const std::vector<onedrive::storage::BlockedItem>& blocked_;
};

class FakeMonitor final {
public:
    FakeMonitor(
        int& run_count,
        bool& keyboard_exit,
        bool& stop_requested,
        onedrive::monitor::SyncCallback synchronize
    )
        : run_count_{run_count},
          keyboard_exit_{keyboard_exit},
          stop_requested_{stop_requested},
          synchronize_{std::move(synchronize)} {
    }

    [[nodiscard]] int run() const {
        ++run_count_;
        static_cast<void>(synchronize_({}));
        return 0;
    }
    [[nodiscard]] int run(bool) const {
        return run();
    }
    [[nodiscard]] int run(const std::stop_token& stop_token) const {
        ++run_count_;
        return synchronize_(stop_token);
    }
    [[nodiscard]] int run(
        bool keyboard_exit, const std::stop_token& stop_token
    ) const {
        keyboard_exit_ = keyboard_exit;
        stop_requested_ = stop_token.stop_requested();
        ++run_count_;
        if (!stop_requested_) {
            static_cast<void>(synchronize_(stop_token));
        }
        return 0;
    }

private:
    int& run_count_;
    bool& keyboard_exit_;
    bool& stop_requested_;
    onedrive::monitor::SyncCallback synchronize_;
};

class FakeMetrics final {
public:
    void record_sync_run(
        onedrive::metrics::SyncRunOutcome, std::chrono::duration<double>
    ) noexcept {
    }
};

class FakeRuntimeFactory final {
public:
    [[nodiscard]] std::unique_ptr<onedrive::http::HttpTransport>
    create_http_transport(const onedrive::config::Config&) const {
        return std::make_unique<onedrive::http::HttpTransport>(
            std::in_place_type<FakeAuthenticationTransport>,
            authentication_response_override
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

    [[nodiscard]] std::unique_ptr<onedrive::auth::TokenStore>
    create_token_store(const onedrive::config::Config& config) const {
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
            std::in_place_type<FakeGraphClient>, configured_drive_id
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::graph::GraphInfoClient>
    create_graph_info_client(const onedrive::config::Config&) const {
        ++graph_info_client_count;
        return std::make_unique<onedrive::graph::GraphInfoClient>(
            std::in_place_type<FakeGraphClient>,
            configured_drive_id,
            empty_shared,
            empty_sites,
            include_configured_drive
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::storage::ItemStore>
    create_item_store(
        const onedrive::config::Config& config,
        const onedrive::account::DriveIdentity& identity
    ) const {
        ++item_store_count;
        last_item_store_sync_directory = config.sync_data_directory;
        if (use_real_item_store) {
            return std::make_unique<onedrive::storage::ItemStore>(
                std::in_place_type<onedrive::storage::ItemDatabase>,
                onedrive::account::AccountState::locate(
                    config.state_directory, identity
                )
                    .drive_directory,
                identity
            );
        }
        return std::make_unique<onedrive::storage::ItemStore>(
            std::in_place_type<FakeItemStore>,
            item_store_open_count,
            item_store_apply_delta_count,
            item_store_reset_count,
            reset_drive_id,
            clear_count_,
            clear_drive_id_,
            item_states,
            partial_download_states,
            pending_download_states,
            blocked_item_states
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::monitor::FileMonitor>
    create_monitor(
        const onedrive::config::Config&,
        onedrive::monitor::SyncCallback synchronize,
        onedrive::graph::GraphClient&
    ) const {
        ++monitor_count;
        auto counted_synchronize = [this,
                                    synchronize = std::move(synchronize)](
                                       const std::stop_token& stop_token
                                   ) {
            ++monitor_sync_count;
            return synchronize(stop_token);
        };
        return std::make_unique<onedrive::monitor::FileMonitor>(
            std::in_place_type<FakeMonitor>,
            monitor_run_count,
            monitor_keyboard_exit,
            monitor_stop_requested,
            std::move(counted_synchronize)
        );
    }

    [[nodiscard]] std::unique_ptr<onedrive::metrics::Metrics> create_metrics(
        const onedrive::config::Config&, const onedrive::account::DriveIdentity&
    ) const {
        ++metrics_count;
        return std::make_unique<onedrive::metrics::Metrics>(
            std::in_place_type<FakeMetrics>
        );
    }

    std::string configured_drive_id{"me"};
    FakeAuthenticationTransport::ResponseOverride
        authentication_response_override;
    bool empty_shared{false};
    bool empty_sites{false};
    bool include_configured_drive{false};
    bool use_real_item_store{false};
    mutable int token_store_count{0};
    mutable int graph_client_count{0};
    mutable int graph_info_client_count{0};
    mutable int item_store_count{0};
    mutable int item_store_open_count{0};
    mutable int item_store_apply_delta_count{0};
    mutable int item_store_reset_count{0};
    mutable std::string reset_drive_id;
    mutable int clear_count_{0};
    mutable std::string clear_drive_id_;
    mutable int monitor_count{0};
    mutable int monitor_run_count{0};
    mutable int monitor_sync_count{0};
    mutable bool monitor_keyboard_exit{false};
    mutable bool monitor_stop_requested{false};
    mutable int metrics_count{0};
    mutable std::filesystem::path last_item_store_sync_directory;
    std::vector<onedrive::storage::ItemState> item_states;
    std::vector<onedrive::storage::PartialDownload>
        partial_download_states;
    std::vector<onedrive::storage::PendingDownload> pending_download_states;
    std::vector<onedrive::storage::BlockedItem> blocked_item_states;
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
