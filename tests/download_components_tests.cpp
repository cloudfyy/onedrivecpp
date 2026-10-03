#include "download_recovery.hpp"
#include "download_integrity.hpp"
#include "download_transaction.hpp"
#include "filesystem_metadata.hpp"
#include "test_support.hpp"

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

using onedrive::test::TemporaryDirectory;

class FakeGraphClient final {
public:
    [[nodiscard]] onedrive::account::DriveIdentity drive_identity()
        const {
        return {
            .user_id = "user-id",
            .user_display_name = "Test User",
            .configured_drive_id = "me",
            .drive_id = "drive-id",
            .drive_name = "Test Drive",
        };
    }

    [[nodiscard]] std::vector<onedrive::graph::RemoteItem>
    list_root() const {
        return {};
    }

    [[nodiscard]] onedrive::graph::DeltaResult list_delta(
        const std::optional<std::string>&,
        const onedrive::graph::DeltaProgress&
    ) const {
        return {};
    }

    void download_file(
        const std::string&,
        std::uint64_t expected_size,
        const std::filesystem::path& destination,
        std::uint64_t initial_offset,
        const onedrive::graph::DownloadProgress&,
        const onedrive::graph::DownloadCheckpoint& checkpoint
    ) const {
        last_initial_offset = initial_offset;
        if (initial_offset == 0) {
            std::ofstream output{destination, std::ios::binary};
            output << contents;
        } else {
            std::fstream output{
                destination,
                std::ios::in | std::ios::out | std::ios::binary
            };
            output.seekp(static_cast<std::streamoff>(initial_offset));
            output << contents.substr(initial_offset);
        }
        if (checkpoint) {
            checkpoint(expected_size);
        }
    }

    std::string contents{"data"};
    mutable std::uint64_t last_initial_offset{0};
};

class FakeItemStore final {
public:
    void open() {}

    void upsert(onedrive::storage::ItemState item) {
        if (fail_upsert) {
            throw std::runtime_error{"simulated persistence failure"};
        }
        states.insert_or_assign(item.remote_id, std::move(item));
    }

    void apply_delta(onedrive::storage::ItemDelta) {}

    void save_pending_download(
        onedrive::storage::PendingDownload download
    ) {
        pending.insert_or_assign(
            download.item.remote_id,
            std::move(download)
        );
    }

    void remove_pending_download(
        const std::string&,
        const std::string& remote_id
    ) {
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

    void save_partial_download(
        onedrive::storage::PartialDownload download
    ) {
        partials.insert_or_assign(
            download.item.remote_id,
            std::move(download)
        );
    }

    void remove_partial_download(
        const std::string&,
        const std::string& remote_id
    ) {
        partials.erase(remote_id);
    }

    [[nodiscard]] std::optional<onedrive::storage::PartialDownload>
    partial_download(
        const std::string&,
        const std::string& remote_id
    ) const {
        const auto iterator = partials.find(remote_id);
        return iterator == partials.end() ?
                   std::nullopt :
                   std::optional<onedrive::storage::PartialDownload>{
                       iterator->second
                   };
    }

    [[nodiscard]] std::vector<onedrive::storage::BlockedItem> blocked_items(
        const std::string&
    ) const {
        return {};
    }

    bool reset(const std::string&) {
        return false;
    }

    onedrive::storage::ClearedState clear(const std::string&) {
        return {};
    }

    [[nodiscard]] std::optional<std::string> delta_link(
        const std::string&
    ) const {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<onedrive::storage::ItemState> find(
        const std::string&,
        const std::string& remote_id
    ) const {
        const auto iterator = states.find(remote_id);
        return iterator == states.end() ?
                   std::nullopt :
                   std::optional<onedrive::storage::ItemState>{
                       iterator->second
                   };
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return states.size();
    }

    std::unordered_map<std::string, onedrive::storage::ItemState> states;
    std::unordered_map<std::string, onedrive::storage::PendingDownload> pending;
    std::unordered_map<std::string, onedrive::storage::PartialDownload> partials;
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
    detail::DownloadSpaceCoordinator space{root, 0};
    FakeGraphClient graph;
    FakeItemStore items;
    auto installed_item = remote_item("installed", "installed.txt");
    installed_item.content_hash = onedrive::FileHash{
        .algorithm = onedrive::FileHashAlgorithm::sha256,
        .value =
            "3A6EB0790F39AC87C94F3856B2DD2C5D110E6811602261A9A923D3BB23ADC8B7",
    };
    const auto destination = root / "installed.txt";
    const auto installed = detail::download_atomically(
        graph,
        items,
        installed_item,
        item_state(installed_item, destination),
        destination,
        metadata,
        space
    );
    if (!std::filesystem::exists(destination) || !items.pending.empty() ||
        !items.partials.empty() || !items.find("me", "installed") ||
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
            metadata,
            space
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
    if (!items.pending.empty() || !items.find("me", "recover") ||
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
            metadata,
            space
        ));
        return fail("download size mismatch was accepted");
    } catch (const std::runtime_error&) {
    }
    if (std::filesystem::exists(mismatch_destination) ||
        items.pending.contains("mismatch")) {
        return fail("size mismatch left installed or journaled state");
    }
    graph.contents = "data";

    const auto truncated_item = remote_item("truncated", "truncated.txt");
    const auto truncated_destination = root / "truncated.txt";
    const auto truncated_partial =
        root / ".truncated.txt.onedrive-partial-previous";
    {
        std::ofstream output{truncated_partial, std::ios::binary};
        output << "dauncommitted";
    }
    items.partials.emplace(
        "truncated",
        onedrive::storage::PartialDownload{
            .item = item_state(truncated_item, truncated_destination),
            .temporary_path = truncated_partial,
            .completed_bytes = 2,
        }
    );
    detail::DownloadSpaceCoordinator resume_space{
        root,
        0,
        [](const std::filesystem::path&) {
            return std::uintmax_t{2};
        }
    };
    static_cast<void>(detail::download_atomically(
        graph,
        items,
        truncated_item,
        item_state(truncated_item, truncated_destination),
        truncated_destination,
        metadata,
        resume_space
    ));
    std::ifstream truncated_input{truncated_destination, std::ios::binary};
    const std::string truncated_contents{
        std::istreambuf_iterator<char>{truncated_input},
        std::istreambuf_iterator<char>{}
    };
    if (graph.last_initial_offset != 2 || truncated_contents != "data" ||
        items.partials.contains("truncated")) {
        return fail(
            "partial download tail was not truncated to its durable checkpoint"
        );
    }

    const auto complete_item = remote_item("complete", "complete.txt");
    const auto complete_destination = root / "complete.txt";
    const auto complete_partial =
        root / ".complete.txt.onedrive-partial-previous";
    {
        std::ofstream output{complete_partial, std::ios::binary};
        output << "data";
    }
    items.partials.emplace(
        "complete",
        onedrive::storage::PartialDownload{
            .item = item_state(complete_item, complete_destination),
            .temporary_path = complete_partial,
            .completed_bytes = 4,
        }
    );
    detail::DownloadSpaceCoordinator complete_space{
        root,
        0,
        [](const std::filesystem::path&) -> std::uintmax_t {
            throw std::runtime_error{
                "zero-byte reservation queried filesystem capacity"
            };
        }
    };
    static_cast<void>(detail::download_atomically(
        graph,
        items,
        complete_item,
        item_state(complete_item, complete_destination),
        complete_destination,
        metadata,
        complete_space
    ));
    if (graph.last_initial_offset != 4 ||
        !std::filesystem::exists(complete_destination) ||
        items.partials.contains("complete")) {
        return fail("fully checkpointed download was not installed");
    }

    const auto stale_item = remote_item("stale", "stale.txt");
    const auto stale_destination = root / "stale.txt";
    const auto stale_partial =
        root / ".stale.txt.onedrive-partial-previous";
    {
        std::ofstream output{stale_partial, std::ios::binary};
        output << "ol";
    }
    auto stale_state = item_state(stale_item, stale_destination);
    stale_state.etag = "old-etag";
    items.partials.emplace(
        "stale",
        onedrive::storage::PartialDownload{
            .item = stale_state,
            .temporary_path = stale_partial,
            .completed_bytes = 2,
        }
    );
    static_cast<void>(detail::download_atomically(
        graph,
        items,
        stale_item,
        item_state(stale_item, stale_destination),
        stale_destination,
        metadata,
        space
    ));
    if (graph.last_initial_offset != 0 ||
        std::filesystem::exists(stale_partial) ||
        items.partials.contains("stale")) {
        return fail("stale partial download was not restarted safely");
    }

    const auto unsafe_item = remote_item("unsafe-partial", "unsafe.txt");
    const auto unsafe_destination = root / "unsafe.txt";
    const auto unsafe_partial =
        temporary.path() / ".unsafe.txt.onedrive-partial-outside";
    {
        std::ofstream output{unsafe_partial, std::ios::binary};
        output << "da";
    }
    items.partials.emplace(
        "unsafe-partial",
        onedrive::storage::PartialDownload{
            .item = item_state(unsafe_item, unsafe_destination),
            .temporary_path = unsafe_partial,
            .completed_bytes = 2,
        }
    );
    try {
        static_cast<void>(detail::download_atomically(
            graph,
            items,
            unsafe_item,
            item_state(unsafe_item, unsafe_destination),
            unsafe_destination,
            metadata,
            space
        ));
        return fail("unsafe partial download path was accepted");
    } catch (const std::runtime_error&) {
    }
    if (!std::filesystem::exists(unsafe_partial) ||
        !items.partials.contains("unsafe-partial") ||
        std::filesystem::exists(unsafe_destination)) {
        return fail("unsafe partial download path changed recovery state");
    }

    auto quick_xor_item = remote_item("quick-xor", "quick-xor.txt");
    quick_xor_item.size = 1;
    quick_xor_item.content_hash = onedrive::FileHash{
        .algorithm = onedrive::FileHashAlgorithm::quick_xor,
        .value = "SgAAAAAAAAAAAAAAAQAAAAAAAAA=",
    };
    const auto quick_xor_destination = root / "quick-xor.txt";
    graph.contents = "J";
    static_cast<void>(detail::download_atomically(
        graph,
        items,
        quick_xor_item,
        item_state(quick_xor_item, quick_xor_destination),
        quick_xor_destination,
        metadata,
        space
    ));
    if (!std::filesystem::exists(quick_xor_destination) ||
        items.partials.contains("quick-xor")) {
        return fail("valid QuickXorHash download was not installed");
    }

    auto corrupt_item = remote_item("corrupt", "corrupt.txt");
    corrupt_item.content_hash = onedrive::FileHash{
        .algorithm = onedrive::FileHashAlgorithm::sha256,
        .value =
            "0000000000000000000000000000000000000000000000000000000000000000",
    };
    const auto corrupt_destination = root / "corrupt.txt";
    graph.contents = "data";
    try {
        static_cast<void>(detail::download_atomically(
            graph,
            items,
            corrupt_item,
            item_state(corrupt_item, corrupt_destination),
            corrupt_destination,
            metadata,
            space
        ));
        return fail("download with a mismatched Graph hash was accepted");
    } catch (const detail::DownloadIntegrityError&) {
    }
    bool corrupt_temporary_exists = false;
    for (const auto& entry : std::filesystem::directory_iterator{root}) {
        if (entry.path().filename().string().starts_with(
                ".corrupt.txt.onedrive-partial-"
            )) {
            corrupt_temporary_exists = true;
        }
    }
    if (std::filesystem::exists(corrupt_destination) ||
        corrupt_temporary_exists || items.partials.contains("corrupt")) {
        return fail("failed hash verification retained resumable state");
    }

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
