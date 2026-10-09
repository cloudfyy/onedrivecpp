#pragma once

#include "onedrive/account/account_state.hpp"
#include "onedrive/util/file_hash.hpp"
#include "onedrive/http/http_options.hpp"
#include "onedrive/util/progress.hpp"
#include "onedrive/util/proxy_service.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <proxy/proxy.h>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace onedrive::auth {
class DeviceAuthClient;
class TokenStore;
struct DeviceAuthOptions;
}

namespace onedrive::http {
class TransferRateLimiter;
class HttpTransport;
struct HttpResponse;
}

namespace onedrive::graph {

using DownloadProgress =
    std::function<void(std::uint64_t downloaded, std::uint64_t total)>;
using DownloadData = std::function<void(
    std::uint64_t offset,
    std::span<const std::byte> data
)>;
using DownloadCheckpoint =
    std::function<void(std::uint64_t completed_bytes)>;
using DeltaProgress = std::function<void(
    std::size_t pages,
    std::size_t items,
    util::ProgressState state
)>;

struct RemoteItem {
    std::string id;
    std::string name;
    std::string etag;
    std::string ctag;
    std::string parent_id;
    std::string remote_path;
    std::string last_modified;
    std::int64_t size{0};
    bool directory{false};
    bool deleted{false};
    bool root{false};
    bool malware{false};
    std::optional<util::FileHash> content_hash;
    bool validate_content{true};
};

struct DeltaResult {
    std::vector<RemoteItem> changes;
    std::string delta_link;
};

struct NotificationChannel {
    std::string notification_url;
    std::chrono::system_clock::time_point expires_at;
};

struct DriveQuota {
    std::uint64_t total{0};
    std::uint64_t used{0};
    std::uint64_t remaining{0};
    std::uint64_t deleted{0};
    std::string state;
};

struct DriveInfo {
    std::string id;
    std::string name;
    std::string type;
    std::string web_url;
    std::string owner;
    std::optional<DriveQuota> quota;
};

enum class SharedResourceSource {
    shared_with_me,
    shortcut,
};

struct SharedResource {
    std::string name;
    std::string target_name;
    std::string drive_id;
    std::string item_id;
    std::string web_url;
    std::string owner;
    std::string local_path;
    bool directory{false};
    SharedResourceSource source{SharedResourceSource::shared_with_me};
};

struct SiteInfo {
    std::string id;
    std::string name;
    std::string display_name;
    std::string web_url;
    std::vector<DriveInfo> drives;
};

enum class NotificationFailureKind {
    transient,
    unauthorized,
};

class NotificationChannelError final : public std::runtime_error {
public:
    NotificationChannelError(
        NotificationFailureKind kind, const std::string& message
    )
        : std::runtime_error{message},
          kind_{kind} {}

    [[nodiscard]] bool unauthorized() const noexcept {
        return kind_ == NotificationFailureKind::unauthorized;
    }

private:
    NotificationFailureKind kind_;
};

class DeltaCursorInvalidError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class DownloadCancelledError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RemoteItemChangedError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class UploadConflictError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class UploadResourceError final : public std::runtime_error {
public:
    UploadResourceError(
        std::string reason_code, const std::string& message
    )
        : std::runtime_error{message},
          reason_code_{std::move(reason_code)} {}

    [[nodiscard]] const std::string& reason_code() const noexcept {
        return reason_code_;
    }

private:
    std::string reason_code_;
};

struct UploadSession {
    std::string upload_url;
    std::string expiration;
    std::uint64_t completed_bytes{0};
};

using UploadCheckpoint = std::function<void(const UploadSession&)>;

struct GraphOptions {
    std::string drive_id{"me"};
    std::string endpoint{"https://graph.microsoft.com/v1.0"};
    std::size_t maximum_throttle_retries{4};
    std::chrono::seconds initial_throttle_delay{1};
    std::chrono::seconds maximum_throttle_delay{300};
    std::chrono::seconds notification_request_timeout{60};
    std::size_t download_maximum_retries{4};
    std::uint64_t download_chunk_threshold_bytes{
        std::uint64_t{8} * 1024U * 1024U
    };
    std::uint64_t download_checkpoint_interval_bytes{
        std::uint64_t{1024} * 1024U
    };
    std::uint64_t simple_upload_threshold_bytes{
        std::uint64_t{250} * 1000U * 1000U
    };
    std::uint64_t upload_chunk_size_bytes{
        std::uint64_t{10} * 1024U * 1024U
    };
    http::DownloadTransportOptions download_transport;
    http::UploadTransportOptions upload_transport;
    bool relaxed_download_validation{false};
    bool private_download_permissions{true};
};

PRO_DEF_MEM_DISPATCH(GraphDriveIdentityDispatch, drive_identity);
PRO_DEF_MEM_DISPATCH(GraphListRootDispatch, list_root);
PRO_DEF_MEM_DISPATCH(GraphItemByPathDispatch, item_by_path);
PRO_DEF_MEM_DISPATCH(GraphListDeltaDispatch, list_delta);
PRO_DEF_MEM_DISPATCH(GraphDownloadFileDispatch, download_file);
PRO_DEF_MEM_DISPATCH(GraphUploadFileDispatch, upload_file);
PRO_DEF_MEM_DISPATCH(GraphCreateDirectoryDispatch, create_directory);
PRO_DEF_MEM_DISPATCH(GraphDeleteItemDispatch, delete_item);
PRO_DEF_MEM_DISPATCH(GraphMoveItemDispatch, move_item);
PRO_DEF_MEM_DISPATCH(GraphNotificationChannelDispatch, notification_channel);
PRO_DEF_MEM_DISPATCH(GraphRefreshAccessTokenDispatch, refresh_access_token);

struct GraphClientFacade : pro::facade_builder
    ::add_convention<
        GraphDriveIdentityDispatch,
        account::DriveIdentity() const
    >
    ::add_convention<
        GraphListRootDispatch,
        std::vector<RemoteItem>() const
    >
    ::add_convention<
        GraphItemByPathDispatch,
        RemoteItem(const std::string&) const
    >
    ::add_convention<
        GraphListDeltaDispatch,
        DeltaResult(
            const std::optional<std::string>&,
            const DeltaProgress&
        ) const
    >
    ::add_convention<
        GraphDownloadFileDispatch,
        void(
            const std::string&,
            const std::string&,
            std::uint64_t,
            const std::filesystem::path&,
            std::uint64_t,
            std::stop_token,
            const DownloadProgress&,
            const DownloadCheckpoint&,
            const DownloadData&
        ) const
    >
    ::add_convention<
        GraphUploadFileDispatch,
        RemoteItem(
            const std::string&,
            const std::optional<std::string>&,
            const std::string&,
            const std::filesystem::path&,
            const std::optional<UploadSession>&,
            const UploadCheckpoint&,
            std::stop_token
        ) const
    >
    ::add_convention<
        GraphCreateDirectoryDispatch,
        RemoteItem(const std::string&) const
    >
    ::add_convention<
        GraphDeleteItemDispatch,
        void(const std::string&, const std::string&) const
    >
    ::add_convention<
        GraphMoveItemDispatch,
        RemoteItem(
            const std::string&,
            const std::string&,
            const std::string&
        ) const
    >
    ::add_convention<
        GraphNotificationChannelDispatch,
        NotificationChannel() const
    >
    ::add_convention<GraphRefreshAccessTokenDispatch, void() const>
    ::build {};

class GraphClient : private onedrive::util::ProxyService<GraphClientFacade> {
    using Base = onedrive::util::ProxyService<GraphClientFacade>;

public:
    using Base::Base;

    [[nodiscard]] account::DriveIdentity drive_identity() const {
        return implementation()->drive_identity();
    }

    [[nodiscard]] std::vector<RemoteItem> list_root() const {
        return implementation()->list_root();
    }

    [[nodiscard]] RemoteItem item_by_path(
        const std::string& remote_path
    ) const {
        return implementation()->item_by_path(remote_path);
    }

    [[nodiscard]] DeltaResult list_delta(
        const std::optional<std::string>& delta_link,
        const DeltaProgress& progress = {}
    ) const {
        return implementation()->list_delta(delta_link, progress);
    }

    [[nodiscard]] NotificationChannel notification_channel() const {
        return implementation()->notification_channel();
    }

    void refresh_access_token() const {
        implementation()->refresh_access_token();
    }

    void download_file(
        const std::string& remote_id,
        const std::string& expected_etag,
        std::uint64_t expected_size,
        const std::filesystem::path& destination,
        std::uint64_t initial_offset,
        std::stop_token stop_token,
        const DownloadProgress& progress,
        const DownloadCheckpoint& checkpoint,
        const DownloadData& data = {}
    ) const {
        implementation()->download_file(
            remote_id,
            expected_etag,
            expected_size,
            destination,
            initial_offset,
            std::move(stop_token),
            progress,
            checkpoint,
            data
        );
    }

    [[nodiscard]] RemoteItem upload_file(
        const std::string& remote_path,
        const std::optional<std::string>& remote_id,
        const std::string& expected_etag,
        const std::filesystem::path& source,
        const std::optional<UploadSession>& session = std::nullopt,
        const UploadCheckpoint& checkpoint = {},
        std::stop_token stop_token = {}
    ) const {
        return implementation()->upload_file(
            remote_path,
            remote_id,
            expected_etag,
            source,
            session,
            checkpoint,
            std::move(stop_token)
        );
    }

    [[nodiscard]] RemoteItem create_directory(
        const std::string& remote_path
    ) const {
        return implementation()->create_directory(remote_path);
    }

    void delete_item(
        const std::string& remote_id,
        const std::string& expected_etag
    ) const {
        implementation()->delete_item(remote_id, expected_etag);
    }

    [[nodiscard]] RemoteItem move_item(
        const std::string& remote_id,
        const std::string& expected_etag,
        const std::string& destination_path
    ) const {
        return implementation()->move_item(
            remote_id,
            expected_etag,
            destination_path
        );
    }

    void download_file(
        const std::string& remote_id,
        const std::string& expected_etag,
        std::uint64_t expected_size,
        const std::filesystem::path& destination,
        const DownloadProgress& progress = {}
    ) const {
        download_file(
            remote_id,
            expected_etag,
            expected_size,
            destination,
            0,
            {},
            progress,
            {},
            {}
        );
    }
};

PRO_DEF_MEM_DISPATCH(GraphListDrivesDispatch, list_drives);
PRO_DEF_MEM_DISPATCH(GraphDriveInfoDispatch, drive_info);
PRO_DEF_MEM_DISPATCH(GraphListSharedDispatch, list_shared_resources);
PRO_DEF_MEM_DISPATCH(GraphSearchSitesDispatch, search_sites);

struct GraphInfoClientFacade : pro::facade_builder
    ::add_convention<
        GraphDriveIdentityDispatch,
        account::DriveIdentity() const
    >
    ::add_convention<
        GraphListDrivesDispatch,
        std::vector<DriveInfo>() const
    >
    ::add_convention<GraphDriveInfoDispatch, DriveInfo() const>
    ::add_convention<
        GraphListSharedDispatch,
        std::vector<SharedResource>() const
    >
    ::add_convention<
        GraphSearchSitesDispatch,
        std::vector<SiteInfo>(const std::string&) const
    >
    ::build {};

class GraphInfoClient
    : private onedrive::util::ProxyService<GraphInfoClientFacade> {
    using Base = onedrive::util::ProxyService<GraphInfoClientFacade>;

public:
    using Base::Base;

    [[nodiscard]] account::DriveIdentity drive_identity() const {
        return implementation()->drive_identity();
    }

    [[nodiscard]] std::vector<DriveInfo> list_drives() const {
        return implementation()->list_drives();
    }

    [[nodiscard]] DriveInfo drive_info() const {
        return implementation()->drive_info();
    }

    [[nodiscard]] std::vector<SharedResource>
    list_shared_resources() const {
        return implementation()->list_shared_resources();
    }

    [[nodiscard]] std::vector<SiteInfo>
    search_sites(const std::string& query) const {
        return implementation()->search_sites(query);
    }
};

class MicrosoftGraphClient final {
public:
    using SleepFunction = std::function<void(std::chrono::seconds)>;

    MicrosoftGraphClient(
        std::unique_ptr<http::HttpTransport> transport,
        std::unique_ptr<auth::TokenStore> token_store,
        auth::DeviceAuthOptions auth_options,
        GraphOptions options = {},
        SleepFunction sleep = {}
    );
    ~MicrosoftGraphClient();
    MicrosoftGraphClient(const MicrosoftGraphClient&) = delete;
    MicrosoftGraphClient& operator=(const MicrosoftGraphClient&) = delete;
    MicrosoftGraphClient(MicrosoftGraphClient&&) = delete;
    MicrosoftGraphClient& operator=(MicrosoftGraphClient&&) = delete;

    [[nodiscard]] account::DriveIdentity drive_identity() const;
    [[nodiscard]] std::vector<DriveInfo> list_drives() const;
    [[nodiscard]] DriveInfo drive_info() const;
    [[nodiscard]] std::vector<SharedResource>
    list_shared_resources() const;
    [[nodiscard]] std::vector<SiteInfo>
    search_sites(const std::string& query) const;
    [[nodiscard]] std::vector<RemoteItem> list_root() const;
    [[nodiscard]] RemoteItem item_by_path(
        const std::string& remote_path
    ) const;
    [[nodiscard]] RemoteItem upload_file(
        const std::string& remote_path,
        const std::optional<std::string>& remote_id,
        const std::string& expected_etag,
        const std::filesystem::path& source,
        const std::optional<UploadSession>& session = std::nullopt,
        const UploadCheckpoint& checkpoint = {},
        std::stop_token stop_token = {}
    ) const;
    void delete_item(
        const std::string& remote_id,
        const std::string& expected_etag
    ) const;
    [[nodiscard]] RemoteItem move_item(
        const std::string& remote_id,
        const std::string& expected_etag,
        const std::string& destination_path
    ) const;
    [[nodiscard]] RemoteItem create_directory(
        const std::string& remote_path
    ) const;
    [[nodiscard]] DeltaResult list_delta(
        const std::optional<std::string>& delta_link,
        const DeltaProgress& progress = {}
    ) const;
    [[nodiscard]] NotificationChannel notification_channel() const;
    void refresh_access_token() const;
    void download_file(
        const std::string& remote_id,
        const std::string& expected_etag,
        std::uint64_t expected_size,
        const std::filesystem::path& destination,
        std::uint64_t initial_offset,
        std::stop_token stop_token,
        const DownloadProgress& progress,
        const DownloadCheckpoint& checkpoint,
        const DownloadData& data = {}
    ) const;
    void download_file(
        const std::string& remote_id,
        const std::string& expected_etag,
        std::uint64_t expected_size,
        const std::filesystem::path& destination,
        const DownloadProgress& progress = {}
    ) const {
        download_file(
            remote_id,
            expected_etag,
            expected_size,
            destination,
            0,
            {},
            progress,
            {},
            {}
        );
    }

private:
    std::unique_ptr<http::HttpTransport> transport_;
    std::unique_ptr<auth::TokenStore> token_store_;
    GraphOptions options_;
    std::unique_ptr<auth::DeviceAuthClient> auth_client_;
    std::unique_ptr<http::TransferRateLimiter> download_rate_limiter_;
    std::unique_ptr<http::TransferRateLimiter> upload_rate_limiter_;
    SleepFunction sleep_;
    mutable std::mutex access_token_mutex_;
    mutable std::string cached_access_token_;
    mutable std::chrono::system_clock::time_point access_token_expires_at_{};

    [[nodiscard]] std::string access_token() const;
    [[nodiscard]] http::HttpResponse graph_get(
        const std::string& url,
        std::string_view description
    ) const;
    void invalidate_access_token() const;
};

[[nodiscard]] account::DriveIdentity fetch_drive_identity(
    const http::HttpTransport& transport,
    std::string_view access_token,
    GraphOptions options = {},
    std::stop_token stop_token = {}
);

}  // namespace onedrive::graph
