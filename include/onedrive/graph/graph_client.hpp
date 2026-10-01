#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
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

struct RemoteItem {
    std::string id;
    std::string name;
    std::string etag;
    bool directory{false};
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

private:
    std::unique_ptr<http::HttpTransport> transport_;
    std::unique_ptr<auth::TokenStore> token_store_;
    GraphOptions options_;
    std::unique_ptr<auth::DeviceAuthClient> auth_client_;
    SleepFunction sleep_;
};

}  // namespace onedrive::graph
