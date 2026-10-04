#pragma once

#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace onedrive::sync::detail {

class SyncPlan {
public:
    [[nodiscard]] static SyncPlan build(
        graph::DeltaResult delta,
        const std::string& drive_id,
        const std::filesystem::path& sync_directory,
        bool replace_drive_items,
        std::string sync_filter_fingerprint
    );

    [[nodiscard]] const graph::RemoteItem& directory(
        std::size_t index
    ) const;
    [[nodiscard]] const graph::RemoteItem& download(
        std::size_t index
    ) const;
    [[nodiscard]] storage::ItemState& state_for(
        const std::string& remote_id
    );
    void block(
        const graph::RemoteItem& item,
        std::string reason_code,
        std::string reason_message
    );
    [[nodiscard]] const storage::BlockedItem& blocked(
        std::size_t index
    ) const;
    [[nodiscard]] storage::ItemDelta release_state_delta();

    [[nodiscard]] std::size_t change_count() const noexcept;
    [[nodiscard]] std::size_t directory_count() const noexcept;
    [[nodiscard]] std::size_t download_count() const noexcept;
    [[nodiscard]] std::size_t removal_count() const noexcept;
    [[nodiscard]] std::size_t blocked_count() const noexcept;
    [[nodiscard]] std::uintmax_t download_bytes() const noexcept;

private:
    graph::DeltaResult delta_;
    storage::ItemDelta state_delta_;
    std::vector<std::size_t> directories_;
    std::vector<std::size_t> downloads_;
    std::uintmax_t download_bytes_{0};
};

}  // namespace onedrive::sync::detail
