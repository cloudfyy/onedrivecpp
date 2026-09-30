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
    [[nodiscard]] std::vector<RemoteItem> list_root() const;
};

}  // namespace onedrive::graph
