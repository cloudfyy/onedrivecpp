#include "onedrive/graph/graph_client.hpp"

#include <stdexcept>

namespace onedrive::graph {

std::vector<RemoteItem> MicrosoftGraphClient::list_root() const {
    throw std::runtime_error(
        "Microsoft Graph drive item requests are not implemented yet; use --dry-run"
    );
}

}  // namespace onedrive::graph
