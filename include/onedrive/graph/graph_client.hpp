#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
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
};

class GraphClient {
public:
    virtual ~GraphClient() = default;

    [[nodiscard]] virtual std::vector<RemoteItem> list_root() const = 0;
    [[nodiscard]] virtual DeltaResult list_delta(
        const std::optional<std::string>& delta_link,
        const DeltaProgress& progress = {}
    ) const = 0;
    virtual void download_file(
        const std::string& remote_id,
        const std::filesystem::path& destination,
        const DownloadProgress& progress = {}
    ) const = 0;
};

class MicrosoftGraphClient final : public GraphClient {
public:
    using SleepFunction = std::function<void(std::chrono::seconds)>;

    MicrosoftGraphClient(
        std::unique_ptr<http::HttpTransport> transport,
        std::unique_ptr<auth::TokenStore> token_store,
        auth::DeviceAuthOptions auth_options,
        GraphOptions options = {},
        SleepFunction sleep = {}
    );
    ~MicrosoftGraphClient() override;

    [[nodiscard]] std::vector<RemoteItem> list_root() const override;
    [[nodiscard]] DeltaResult list_delta(
        const std::optional<std::string>& delta_link,
        const DeltaProgress& progress = {}
    ) const override;
    void download_file(
        const std::string& remote_id,
        const std::filesystem::path& destination,
        const DownloadProgress& progress = {}
    ) const override;

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

}  // namespace onedrive::graph
