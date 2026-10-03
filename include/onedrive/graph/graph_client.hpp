#pragma once

#include "onedrive/account/account_state.hpp"
#include "onedrive/file_hash.hpp"
#include "onedrive/proxy_service.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <proxy/proxy.h>
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
class HttpTransport;
}

namespace onedrive::graph {

using DownloadProgress =
    std::function<void(std::uint64_t downloaded, std::uint64_t total)>;
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
    std::string parent_id;
    std::string remote_path;
    std::string last_modified;
    std::int64_t size{0};
    bool directory{false};
    bool deleted{false};
    bool root{false};
    std::optional<FileHash> content_hash;
};

struct DeltaResult {
    std::vector<RemoteItem> changes;
    std::string delta_link;
};

class DeltaCursorInvalidError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct GraphOptions {
    std::string drive_id{"me"};
    std::string endpoint{"https://graph.microsoft.com/v1.0"};
    std::size_t maximum_throttle_retries{4};
    std::chrono::seconds initial_throttle_delay{1};
    std::chrono::seconds maximum_throttle_delay{300};
    std::uint64_t download_chunk_threshold_bytes{
        std::uint64_t{8} * 1024U * 1024U
    };
};

PRO_DEF_MEM_DISPATCH(GraphDriveIdentityDispatch, drive_identity);
PRO_DEF_MEM_DISPATCH(GraphListRootDispatch, list_root);
PRO_DEF_MEM_DISPATCH(GraphListDeltaDispatch, list_delta);
PRO_DEF_MEM_DISPATCH(GraphDownloadFileDispatch, download_file);

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
            std::uint64_t,
            const std::filesystem::path&,
            std::uint64_t,
            const DownloadProgress&,
            const DownloadCheckpoint&
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

    [[nodiscard]] DeltaResult list_delta(
        const std::optional<std::string>& delta_link,
        const DeltaProgress& progress = {}
    ) const {
        return implementation()->list_delta(delta_link, progress);
    }

    void download_file(
        const std::string& remote_id,
        std::uint64_t expected_size,
        const std::filesystem::path& destination,
        std::uint64_t initial_offset,
        const DownloadProgress& progress,
        const DownloadCheckpoint& checkpoint
    ) const {
        implementation()->download_file(
            remote_id,
            expected_size,
            destination,
            initial_offset,
            progress,
            checkpoint
        );
    }

    void download_file(
        const std::string& remote_id,
        std::uint64_t expected_size,
        const std::filesystem::path& destination,
        const DownloadProgress& progress = {}
    ) const {
        download_file(
            remote_id,
            expected_size,
            destination,
            0,
            progress,
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
    [[nodiscard]] DeltaResult list_delta(
        const std::optional<std::string>& delta_link,
        const DeltaProgress& progress = {}
    ) const;
    void download_file(
        const std::string& remote_id,
        std::uint64_t expected_size,
        const std::filesystem::path& destination,
        std::uint64_t initial_offset,
        const DownloadProgress& progress,
        const DownloadCheckpoint& checkpoint
    ) const;
    void download_file(
        const std::string& remote_id,
        std::uint64_t expected_size,
        const std::filesystem::path& destination,
        const DownloadProgress& progress = {}
    ) const {
        download_file(
            remote_id,
            expected_size,
            destination,
            0,
            progress,
            {}
        );
    }

private:
    std::unique_ptr<http::HttpTransport> transport_;
    std::unique_ptr<auth::TokenStore> token_store_;
    GraphOptions options_;
    std::unique_ptr<auth::DeviceAuthClient> auth_client_;
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
