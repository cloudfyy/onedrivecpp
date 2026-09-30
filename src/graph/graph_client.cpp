#include "onedrive/graph/graph_client.hpp"

#include <stdexcept>

namespace onedrive::graph {

std::vector<RemoteItem> GraphClient::list_root() const {
    throw std::runtime_error(
        "Microsoft Graph authentication and transport are not implemented yet; use --dry-run"
    );
}

}  // namespace onedrive::graph
