#include "onedrive/sync/download/single_file.hpp"
#include "sync/download/integrity.hpp"
#include "sync/download/recovery.hpp"
#include "sync/download/transaction.hpp"
#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/safe_backup.hpp"
#include "support/sync.hpp"
#include "support/common.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace onedrive::test::download {

using onedrive::test::TemporaryDirectory;

struct TestJournaledDownloadState final
    : onedrive::sync::detail::DownloadTransactionState {};
struct InvalidDownloadState {};

using TestJournaledDownload =
    onedrive::sync::detail::DownloadTransaction<TestJournaledDownloadState>;

template <typename Download>
concept CommittableDownload = requires(
    onedrive::storage::ItemStore& items,
    const onedrive::sync::detail::FilesystemMetadata& metadata,
    Download download
) {
    onedrive::sync::detail::commit_download(
        items, metadata, std::move(download)
    );
};

template <typename Download>
concept DiscardablePreparedDownload = requires(const Download& download) {
    onedrive::sync::detail::discard_prepared_download(download);
};

static_assert(onedrive::sync::detail::DownloadState<
              onedrive::sync::detail::DownloadPreparedState>);
static_assert(onedrive::sync::detail::DownloadState<TestJournaledDownloadState>
);
static_assert(!onedrive::sync::detail::DownloadState<InvalidDownloadState>);
static_assert(CommittableDownload<onedrive::sync::detail::PreparedDownload>);
static_assert(!CommittableDownload<TestJournaledDownload>);
static_assert(
    DiscardablePreparedDownload<onedrive::sync::detail::PreparedDownload>
);
static_assert(!DiscardablePreparedDownload<TestJournaledDownload>);

class FakeGraphClient final {
public:
    [[nodiscard]] onedrive::graph::NotificationChannel
    notification_channel() const {
        return {
            .notification_url = "https://notification.example.test/token",
            .expires_at =
                std::chrono::system_clock::now() + std::chrono::hours{1},
        };
    }

    void refresh_access_token() const {
    }

    [[nodiscard]] onedrive::account::DriveIdentity drive_identity() const {
        return onedrive::test::test_drive_identity();
    }

    [[nodiscard]] std::vector<onedrive::graph::RemoteItem> list_root() const {
        return {};
    }

    [[nodiscard]] onedrive::graph::RemoteItem
    item_by_path(const std::string& path) const {
        ++path_lookup_count;
        if (path != lookup_item.remote_path) {
            throw std::runtime_error{"unexpected single path lookup"};
        }
        return lookup_item;
    }

    [[nodiscard]] onedrive::graph::DeltaResult list_delta(
        const std::optional<std::string>&, const onedrive::graph::DeltaProgress&
    ) const {
        return {};
    }

    void download_file(
        const std::string&,
        const std::string& expected_etag,
        std::uint64_t expected_size,
        const std::filesystem::path& destination,
        std::uint64_t initial_offset,
        std::stop_token,
        const onedrive::graph::DownloadProgress& progress,
        const onedrive::graph::DownloadCheckpoint& checkpoint,
        const onedrive::graph::DownloadData& data
    ) const {
        ++download_count;
        last_expected_etag = expected_etag;
        last_initial_offset = initial_offset;
        if (initial_offset == 0) {
            std::ofstream output{destination, std::ios::binary};
            output << contents;
        } else {
            std::fstream output{
                destination, std::ios::in | std::ios::out | std::ios::binary
            };
            output.seekp(static_cast<std::streamoff>(initial_offset));
            output << contents.substr(initial_offset);
        }
        if (data) {
            const std::string_view downloaded{contents};
            data(
                initial_offset,
                std::as_bytes(
                    std::span{downloaded.substr(
                        static_cast<std::size_t>(initial_offset)
                    )}
                )
            );
        }
        if (progress) {
            if (progress_updates.empty()) {
                progress(contents.size(), contents.size());
            } else {
                for (const auto downloaded : progress_updates) {
                    progress(downloaded, contents.size());
                }
            }
        }
        if (send_checkpoint && checkpoint) {
            checkpoint(expected_size);
        }
    }

    [[nodiscard]] onedrive::graph::RemoteItem upload_file(
        const std::string&,
        const std::optional<std::string>&,
        const std::string&,
        const std::filesystem::path&,
        const std::optional<onedrive::graph::UploadSession>&,
        const onedrive::graph::UploadCheckpoint&,
        std::stop_token
    ) const {
        throw std::logic_error{"upload was not expected"};
    }

    [[nodiscard]] onedrive::graph::RemoteItem
    create_directory(const std::string&) const {
        throw std::logic_error{"directory creation was not expected"};
    }

    void delete_item(const std::string&, const std::string&) const {
    }
    [[nodiscard]] onedrive::graph::RemoteItem move_item(
        const std::string&, const std::string&, const std::string&
    ) const {
        return {};
    }

    std::string contents{"data"};
    std::vector<std::uint64_t> progress_updates;
    bool send_checkpoint{true};
    onedrive::graph::RemoteItem lookup_item;
    mutable int path_lookup_count{0};
    mutable int download_count{0};
    mutable std::string last_expected_etag;
    mutable std::uint64_t last_initial_offset{0};
};

class FakeItemStore final {
public:
    void open() {
    }

    void open_read_only() {
    }

    void upsert(onedrive::storage::ItemState item) {
        if (fail_upsert) {
            throw std::runtime_error{"simulated persistence failure"};
        }
        states.insert_or_assign(item.remote_id, std::move(item));
    }

    void apply_delta(onedrive::storage::ItemDelta) {
    }

    void save_pending_download(onedrive::storage::PendingDownload download) {
        pending.insert_or_assign(download.item.remote_id, std::move(download));
        if (pending_download_saved) {
            pending_download_saved();
        }
    }

    void
    remove_pending_download(const std::string&, const std::string& remote_id) {
        pending.erase(remote_id);
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingDownload>
    pending_downloads(const std::string&) const {
        std::vector<onedrive::storage::PendingDownload> result;
        for (const auto& [remote_id, download] : pending) {
            static_cast<void>(remote_id);
            result.push_back(download);
        }
        return result;
    }

    void save_partial_download(onedrive::storage::PartialDownload download) {
        partials.insert_or_assign(download.item.remote_id, std::move(download));
    }

    void
    remove_partial_download(const std::string&, const std::string& remote_id) {
        partials.erase(remote_id);
    }

    [[nodiscard]] std::optional<onedrive::storage::PartialDownload>
    partial_download(const std::string&, const std::string& remote_id) const {
        const auto iterator = partials.find(remote_id);
        return iterator == partials.end()
                   ? std::nullopt
                   : std::optional<onedrive::storage::PartialDownload>{
                         iterator->second
                     };
    }

    [[nodiscard]] std::vector<onedrive::storage::PartialDownload>
    partial_downloads(const std::string&) const {
        std::vector<onedrive::storage::PartialDownload> result;
        result.reserve(partials.size());
        for (const auto& [remote_id, download] : partials) {
            static_cast<void>(remote_id);
            result.push_back(download);
        }
        return result;
    }

    void save_pending_upload(onedrive::storage::PendingUpload) {
    }
    void remove_pending_upload(const std::string&, const std::string&) {
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingUpload>
    pending_uploads(const std::string&) const {
        return {};
    }

    void commit_upload(
        const onedrive::storage::PendingUpload&,
        onedrive::storage::ItemState item
    ) {
        upsert(std::move(item));
    }

    void save_pending_delete(onedrive::storage::PendingDelete) {
    }
    void remove_pending_delete(const std::string&, const std::string&) {
    }
    [[nodiscard]] std::vector<onedrive::storage::PendingDelete>
    pending_deletes(const std::string&) const {
        return {};
    }
    void commit_delete(const onedrive::storage::PendingDelete&) {
    }
    void save_pending_remote_move(onedrive::storage::PendingRemoteMove) {
    }
    void remove_pending_remote_move(const std::string&, const std::string&) {
    }
    [[nodiscard]] std::vector<onedrive::storage::PendingRemoteMove>
    pending_remote_moves(const std::string&) const {
        return {};
    }
    void commit_remote_move(
        const onedrive::storage::PendingRemoteMove&,
        onedrive::storage::ItemState
    ) {
    }

    void save_pending_move(onedrive::storage::PendingMove) {
    }

    void remove_pending_move(const std::string&, const std::string&) {
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingMove>
    pending_moves(const std::string&) const {
        return {};
    }

    [[nodiscard]] std::vector<onedrive::storage::UploadSuppression>
    upload_suppressions(const std::string&) const {
        return {};
    }

    void remove_upload_suppression(
        const std::string&, const std::filesystem::path&
    ) {
    }

    [[nodiscard]] std::vector<onedrive::storage::BlockedItem>
    blocked_items(const std::string&) const {
        return {};
    }

    bool reset(const std::string&) {
        return false;
    }

    onedrive::storage::ClearedState clear(const std::string&) {
        return {};
    }

    [[nodiscard]] std::optional<std::string>
    delta_link(const std::string&) const {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string>
    sync_filter_fingerprint(const std::string&) const {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<onedrive::storage::ItemState>
    find(const std::string&, const std::string& remote_id) const {
        const auto iterator = states.find(remote_id);
        return iterator == states.end()
                   ? std::nullopt
                   : std::optional<onedrive::storage::ItemState>{
                         iterator->second
                     };
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return states.size();
    }

    [[nodiscard]] std::vector<onedrive::storage::ItemState>
    drive_items(const std::string&) const {
        std::vector<onedrive::storage::ItemState> result;
        result.reserve(states.size());
        for (const auto& [remote_id, item] : states) {
            static_cast<void>(remote_id);
            result.push_back(item);
        }
        return result;
    }

    std::unordered_map<std::string, onedrive::storage::ItemState> states;
    std::unordered_map<std::string, onedrive::storage::PendingDownload> pending;
    std::unordered_map<std::string, onedrive::storage::PartialDownload>
        partials;
    std::function<void()> pending_download_saved;
    bool fail_upsert{false};
};

using onedrive::test::fail;

onedrive::graph::RemoteItem remote_item(std::string id, std::string path) {
    return {
        .id = std::move(id),
        .name = std::filesystem::path{path}.filename().string(),
        .etag = "etag",
        .parent_id = "parent",
        .remote_path = std::move(path),
        .last_modified = "2026-10-02T00:00:00Z",
        .size = 4,
    };
}

onedrive::storage::ItemState item_state(
    const onedrive::graph::RemoteItem& item,
    const std::filesystem::path& destination
) {
    return {
        .drive_id = "me",
        .remote_id = item.id,
        .parent_id = item.parent_id,
        .name = item.name,
        .etag = item.etag,
        .remote_path = item.remote_path,
        .local_path = destination,
        .last_modified = item.last_modified,
        .size = item.size,
    };
}

bool has_remote_modified_time(const std::filesystem::path& path) {
    struct stat status{};
    if (::stat(path.c_str(), &status) == -1) {
        return false;
    }
    const auto expected = std::chrono::sys_days{
        std::chrono::year{2026} / std::chrono::October / 2
    };
    return status.st_mtim.tv_sec ==
           std::chrono::duration_cast<std::chrono::seconds>(
               expected.time_since_epoch()
           )
               .count();
}

inline std::filesystem::path test_root(TemporaryDirectory& temporary) {
    auto root = temporary.path() / "files";
    std::filesystem::create_directories(root);
    return root;
}

struct DownloadFixture final {
    TemporaryDirectory temporary;
    std::filesystem::path root{test_root(temporary)};
    onedrive::sync::detail::FilesystemMetadata metadata{
        onedrive::sync::detail::FilesystemMetadata::detect(
            onedrive::config::FilesystemMetadataMode::database, root
        )
    };
    onedrive::sync::detail::SafeSyncRoot safe_root{root};
    onedrive::sync::detail::DownloadSpaceCoordinator space{root, 0};
    FakeGraphClient graph;
    FakeItemStore items;
};

} // namespace onedrive::test::download
