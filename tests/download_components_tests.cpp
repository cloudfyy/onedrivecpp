#include "download_recovery.hpp"
#include "download_transaction.hpp"
#include "filesystem_metadata.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_{
              std::filesystem::temp_directory_path() /
              ("onedrive-cpp-download-components-" +
               std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()
               ))
          } {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class FakeGraphClient final : public onedrive::graph::GraphClient {
public:
    [[nodiscard]] onedrive::account::DriveIdentity drive_identity()
        const override {
        return {
            .user_id = "user-id",
            .user_display_name = "Test User",
            .configured_drive_id = "me",
            .drive_id = "drive-id",
            .drive_name = "Test Drive",
        };
    }

    [[nodiscard]] std::vector<onedrive::graph::RemoteItem>
    list_root() const override {
        return {};
    }

    [[nodiscard]] onedrive::graph::DeltaResult list_delta(
        const std::optional<std::string>&,
        const onedrive::graph::DeltaProgress&
    ) const override {
        return {};
    }

    void download_file(
        const std::string&,
        const std::filesystem::path& destination,
        const onedrive::graph::DownloadProgress&
    ) const override {
        std::ofstream output{destination, std::ios::binary};
        output << contents;
    }

    std::string contents{"data"};
};

class FakeItemStore final : public onedrive::storage::ItemStore {
public:
    void open() override {}

    void upsert(onedrive::storage::ItemState item) override {
        if (fail_upsert) {
            throw std::runtime_error{"simulated persistence failure"};
        }
        states.insert_or_assign(item.remote_id, std::move(item));
    }

    void apply_delta(onedrive::storage::ItemDelta) override {}

    void save_pending_download(
        onedrive::storage::PendingDownload download
    ) override {
        pending.insert_or_assign(
            download.item.remote_id,
            std::move(download)
        );
    }

    void remove_pending_download(
        const std::string&,
        const std::string& remote_id
    ) override {
        pending.erase(remote_id);
    }

    [[nodiscard]] std::vector<onedrive::storage::PendingDownload>
    pending_downloads(const std::string&) const override {
        std::vector<onedrive::storage::PendingDownload> result;
        for (const auto& [remote_id, download] : pending) {
            static_cast<void>(remote_id);
            result.push_back(download);
        }
        return result;
    }

    [[nodiscard]] std::vector<onedrive::storage::BlockedItem> blocked_items(
        const std::string&
    ) const override {
        return {};
    }

    bool reset(const std::string&) override {
        return false;
    }

    onedrive::storage::ClearedState clear(const std::string&) override {
        return {};
    }

    [[nodiscard]] std::optional<std::string> delta_link(
        const std::string&
    ) const override {
        return std::nullopt;
    }

    [[nodiscard]] const onedrive::storage::ItemState* find(
        const std::string&,
        const std::string& remote_id
    ) const override {
        const auto iterator = states.find(remote_id);
        return iterator == states.end() ? nullptr : &iterator->second;
    }

    [[nodiscard]] std::size_t size() const noexcept override {
        return states.size();
    }

    std::unordered_map<std::string, onedrive::storage::ItemState> states;
    std::unordered_map<std::string, onedrive::storage::PendingDownload> pending;
    bool fail_upsert{false};
};

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

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

}  // namespace

int main() {
    namespace detail = onedrive::sync::detail;

    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    std::filesystem::create_directories(root);
    const auto metadata = detail::FilesystemMetadata::detect(
        onedrive::config::FilesystemMetadataMode::database,
        root
    );
    FakeGraphClient graph;
    FakeItemStore items;
    const auto installed_item = remote_item("installed", "installed.txt");
    const auto destination = root / "installed.txt";
    const auto installed = detail::download_atomically(
        graph,
        items,
        installed_item,
        item_state(installed_item, destination),
        destination,
        metadata
    );
    if (!std::filesystem::exists(destination) || !items.pending.empty() ||
        items.find("me", "installed") == nullptr ||
        installed.local_size != 4 || installed.local_modified_ticks == 0) {
        return fail("atomic download transaction did not commit");
    }

    const auto recover_item = remote_item("recover", "recover.txt");
    const auto recover_destination = root / "recover.txt";
    items.fail_upsert = true;
    try {
        static_cast<void>(detail::download_atomically(
            graph,
            items,
            recover_item,
            item_state(recover_item, recover_destination),
            recover_destination,
            metadata
        ));
        return fail("post-install persistence failure was accepted");
    } catch (const std::runtime_error&) {
    }
    if (!std::filesystem::exists(recover_destination) ||
        items.pending.size() != 1) {
        return fail("failed transaction did not preserve recovery journal");
    }

    items.fail_upsert = false;
    detail::recover_pending_downloads(items, root, "me", metadata);
    if (!items.pending.empty() || items.find("me", "recover") == nullptr ||
        !std::filesystem::exists(recover_destination)) {
        return fail("pending installed download was not recovered");
    }

    graph.contents = "short";
    const auto mismatch_item = remote_item("mismatch", "mismatch.txt");
    const auto mismatch_destination = root / "mismatch.txt";
    try {
        static_cast<void>(detail::download_atomically(
            graph,
            items,
            mismatch_item,
            item_state(mismatch_item, mismatch_destination),
            mismatch_destination,
            metadata
        ));
        return fail("download size mismatch was accepted");
    } catch (const std::runtime_error&) {
    }
    if (std::filesystem::exists(mismatch_destination) ||
        items.pending.contains("mismatch")) {
        return fail("size mismatch left installed or journaled state");
    }
    graph.contents = "data";

    const auto duplicate_destination = root / "duplicate.txt";
    const auto duplicate_temporary = root / ".duplicate.partial";
    for (const auto& path : {duplicate_destination, duplicate_temporary}) {
        std::ofstream output{path, std::ios::binary};
        output << "data";
    }
    items.pending.emplace(
        "duplicate",
        onedrive::storage::PendingDownload{
            .item = {
                .drive_id = "me",
                .remote_id = "duplicate",
                .name = "duplicate.txt",
                .etag = "etag",
                .remote_path = "duplicate.txt",
                .local_path = duplicate_destination,
                .size = 4,
            },
            .temporary_path = duplicate_temporary,
            .content_fingerprint =
                "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602261a9a923d3bb23adc8b7",
        }
    );
    detail::recover_pending_downloads(items, root, "me", metadata);
    if (std::filesystem::exists(duplicate_temporary) ||
        items.pending.contains("duplicate")) {
        return fail("duplicate recovery temporary file was not cleaned");
    }

    items.pending.emplace(
        "invalid",
        onedrive::storage::PendingDownload{
            .item = {
                .drive_id = "me",
                .remote_id = "invalid",
                .name = "invalid.txt",
                .etag = "etag",
                .remote_path = "invalid.txt",
                .local_path = root / "invalid.txt",
                .size = 4,
            },
            .temporary_path = root / ".invalid.partial",
            .content_fingerprint = "not-a-sha256",
        }
    );
    try {
        detail::recover_pending_downloads(items, root, "me", metadata);
        return fail("invalid recovery journal metadata was accepted");
    } catch (const std::runtime_error&) {
    }
    items.pending.erase("invalid");

    items.pending.emplace(
        "outside",
        onedrive::storage::PendingDownload{
            .item = {
                .drive_id = "me",
                .remote_id = "outside",
                .name = "outside.txt",
                .etag = "etag",
                .remote_path = "outside.txt",
                .local_path = temporary.path() / "outside.txt",
                .size = 4,
            },
            .temporary_path = temporary.path() / ".outside.partial",
            .content_fingerprint =
                "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602261a9a923d3bb23adc8b7",
        }
    );
    try {
        detail::recover_pending_downloads(items, root, "me", metadata);
        return fail("recovery destination outside the sync root was accepted");
    } catch (const std::runtime_error&) {
    }
    return EXIT_SUCCESS;
}
