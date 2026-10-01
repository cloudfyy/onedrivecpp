#pragma once

#include <string>
#include <vector>

namespace onedrive::graph {

struct RemoteItem {
    std::string id;
    std::string name;
    std::string etag;
    bool directory{false};
};

class GraphClient {
public:
    virtual ~GraphClient() = default;

    [[nodiscard]] virtual std::vector<RemoteItem> list_root() const = 0;
};

class MicrosoftGraphClient final : public GraphClient {
public:
    [[nodiscard]] std::vector<RemoteItem> list_root() const override;
};

}  // namespace onedrive::graph
