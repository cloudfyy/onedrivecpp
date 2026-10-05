#pragma once

#include "onedrive/account/account_state.hpp"
#include "onedrive/file_hash.hpp"
#include "onedrive/http/http_options.hpp"
#include "onedrive/proxy_service.hpp"

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
class DownloadRateLimiter;
class HttpTransport;
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
    bool completed
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
    std::optional<FileHash> content_hash;
    bool validate_content{true};
};

struct DeltaResult {
    std::vector<RemoteItem> changes;
    std::string delta_link;
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
            const UploadCheckpoint&
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
    ::build {};

class GraphClient : private detail::ProxyService<GraphClientFacade> {
    using Base = detail::ProxyService<GraphClientFacade>;

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
        const UploadCheckpoint& checkpoint = {}
    ) const {
        return implementation()->upload_file(
            remote_path,
            remote_id,
            expected_etag,
            source,
            session,
            checkpoint
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
        const UploadCheckpoint& checkpoint = {}
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
    std::unique_ptr<http::DownloadRateLimiter> download_rate_limiter_;
    SleepFunction sleep_;
    mutable std::mutex access_token_mutex_;
    mutable std::string cached_access_token_;
    mutable std::chrono::system_clock::time_point access_token_expires_at_{};

    [[nodiscard]] std::string access_token() const;
};

[[nodiscard]] account::DriveIdentity fetch_drive_identity(
    const http::HttpTransport& transport,
    std::string_view access_token,
    GraphOptions options = {}
);

}  // namespace onedrive::graph
