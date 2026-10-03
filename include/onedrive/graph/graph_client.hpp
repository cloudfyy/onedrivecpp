#pragma once

#include "onedrive/account/account_state.hpp"

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
            const DownloadProgress&
        ) const
    >
    ::build {};

class GraphClient {
public:
    template <typename Implementation, typename... Args>
    explicit GraphClient(
        std::in_place_type_t<Implementation>,
        Args&&... args
    )
        : implementation_{pro::make_proxy<
              GraphClientFacade,
              Implementation
          >(std::forward<Args>(args)...)} {}

    template <typename Implementation>
    explicit GraphClient(std::unique_ptr<Implementation> implementation)
        : implementation_{std::move(implementation)} {}

    template <typename Implementation>
    explicit GraphClient(Implementation& implementation)
        : implementation_{&implementation} {}

    ~GraphClient() = default;
    GraphClient(const GraphClient&) = delete;
    GraphClient& operator=(const GraphClient&) = delete;
    GraphClient(GraphClient&&) noexcept = default;
    GraphClient& operator=(GraphClient&&) noexcept = default;

    [[nodiscard]] account::DriveIdentity drive_identity() const {
        return implementation_->drive_identity();
    }

    [[nodiscard]] std::vector<RemoteItem> list_root() const {
        return implementation_->list_root();
    }

    [[nodiscard]] DeltaResult list_delta(
        const std::optional<std::string>& delta_link,
        const DeltaProgress& progress = {}
    ) const {
        return implementation_->list_delta(delta_link, progress);
    }

    void download_file(
        const std::string& remote_id,
        std::uint64_t expected_size,
        const std::filesystem::path& destination,
        const DownloadProgress& progress = {}
    ) const {
        implementation_->download_file(
            remote_id,
            expected_size,
            destination,
            progress
        );
    }

private:
    pro::proxy<GraphClientFacade> implementation_;
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
        const DownloadProgress& progress = {}
    ) const;

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
