#pragma once

#include "sync/download/space_coordinator.hpp"
#include "sync/filesystem/local.hpp"
#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"

#include <concepts>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace onedrive::sync::detail {

struct DownloadTransactionState {};
struct DownloadPreparedState final : DownloadTransactionState {};

template <typename State>
concept DownloadState = std::derived_from<State, DownloadTransactionState>;

template <DownloadState State> struct DownloadTransaction {
    using state_type = State;

    graph::RemoteItem item;
    storage::ItemState state;
    std::filesystem::path destination;
    std::filesystem::path temporary_path;
    std::string content_fingerprint;
    std::uintmax_t downloaded_size{0};
    LocalFileBaseline destination_baseline;
    DownloadSpaceCoordinator::Lease space_reservation;
};

using PreparedDownload = DownloadTransaction<DownloadPreparedState>;

static_assert(std::movable<PreparedDownload>);
static_assert(!std::copyable<PreparedDownload>);
static_assert(std::is_nothrow_move_constructible_v<PreparedDownload>);

struct DownloadCommitOptions {
    config::LocalConflictPolicy local_conflict{
        config::LocalConflictPolicy::block
    };
    bool preserve_local{false};
    std::function<void(const std::filesystem::path&)> backup_created;
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
    const SafeSyncRoot& sync_root,
    const FilesystemMetadata& metadata,
    PreparedDownload download,
    DownloadCommitOptions options = {}
);
[[nodiscard]] storage::ItemState commit_download(
    storage::ItemStore& items,
    const FilesystemMetadata& metadata,
    PreparedDownload download,
    DownloadCommitOptions options = {}
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
        onedrive::util::borrowed_proxy,
        graph
    };
    storage::ItemStore store_proxy{
        onedrive::util::borrowed_proxy,
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
