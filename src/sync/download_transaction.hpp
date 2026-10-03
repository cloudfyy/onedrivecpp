#pragma once

#include "download_space_coordinator.hpp"
#include "filesystem_metadata.hpp"
#include "local_filesystem.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"

#include <filesystem>
#include <stop_token>
#include <utility>

namespace onedrive::sync::detail {

struct PreparedDownload {
    graph::RemoteItem item;
    storage::ItemState state;
    std::filesystem::path destination;
    std::filesystem::path temporary_path;
    std::string content_fingerprint;
    std::uintmax_t downloaded_size{0};
    LocalFileBaseline destination_baseline;
    DownloadSpaceCoordinator::Lease space_reservation;
};

[[nodiscard]] PreparedDownload prepare_download(
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const graph::RemoteItem& item,
    storage::ItemState state,
    const std::filesystem::path& destination,
    LocalFileBaseline destination_baseline,
    const FilesystemMetadata& metadata,
    DownloadSpaceCoordinator& space,
    std::stop_token stop_token,
    const graph::DownloadProgress& progress = {}
);
[[nodiscard]] storage::ItemState commit_download(
    storage::ItemStore& items,
    PreparedDownload download
);
void discard_prepared_download(const PreparedDownload& download) noexcept;

[[nodiscard]] storage::ItemState download_atomically(
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const graph::RemoteItem& item,
    storage::ItemState state,
    const std::filesystem::path& destination,
    const FilesystemMetadata& metadata,
    DownloadSpaceCoordinator& space,
    const graph::DownloadProgress& progress = {}
);

template <typename GraphImplementation, typename StoreImplementation>
[[nodiscard]] storage::ItemState download_atomically(
    GraphImplementation& graph,
    StoreImplementation& items,
    const graph::RemoteItem& item,
    storage::ItemState state,
    const std::filesystem::path& destination,
    const FilesystemMetadata& metadata,
    DownloadSpaceCoordinator& space,
    const graph::DownloadProgress& progress = {}
) {
    graph::GraphClient graph_proxy{
        onedrive::detail::borrowed_proxy,
        graph
    };
    storage::ItemStore store_proxy{
        onedrive::detail::borrowed_proxy,
        items
    };
    return download_atomically(
        graph_proxy,
        store_proxy,
        item,
        std::move(state),
        destination,
        metadata,
        space,
        progress
    );
}

}  // namespace onedrive::sync::detail
