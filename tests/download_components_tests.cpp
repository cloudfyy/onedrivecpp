#include "download_recovery.hpp"
#include "download_integrity.hpp"
#include "download_transaction.hpp"
#include "filesystem_metadata.hpp"
#include "safe_backup.hpp"
#include "onedrive/sync/single_file_download.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
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

    [[nodiscard]] onedrive::graph::RemoteItem item_by_path(
        const std::string& path
    ) const {
        ++path_lookup_count;
        if (path != lookup_item.remote_path) {
            throw std::runtime_error{"unexpected single path lookup"};
        }
        return lookup_item;
    }

    [[nodiscard]] onedrive::graph::DeltaResult list_delta(
        const std::optional<std::string>&,
        const onedrive::graph::DeltaProgress&
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
                destination,
                std::ios::in | std::ios::out | std::ios::binary
            };
            output.seekp(static_cast<std::streamoff>(initial_offset));
            output << contents.substr(initial_offset);
        }
        if (data) {
            const std::string_view downloaded{contents};
            data(
                initial_offset,
                std::as_bytes(std::span{
                    downloaded.substr(
                        static_cast<std::size_t>(initial_offset)
                    )
                })
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
        const std::filesystem::path&
    ) const {
        throw std::logic_error{"upload was not expected"};
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

    void save_pending_upload(onedrive::storage::PendingUpload) {}

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

    void save_pending_move(onedrive::storage::PendingMove) {}

    void remove_pending_move(const std::string&, const std::string&) {}

    [[nodiscard]] std::vector<onedrive::storage::PendingMove>
    pending_moves(const std::string&) const {
        return {};
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

    [[nodiscard]] std::optional<std::string> sync_filter_fingerprint(
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

    [[nodiscard]] std::vector<onedrive::storage::ItemState> drive_items(
        const std::string&
    ) const {
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
    std::unordered_map<std::string, onedrive::storage::PartialDownload> partials;
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
    struct stat status {};
    if (::stat(path.c_str(), &status) == -1) {
        return false;
    }
    const auto expected =
        std::chrono::sys_days{
            std::chrono::year{2026} / std::chrono::October / 2
        };
    return status.st_mtim.tv_sec ==
           std::chrono::duration_cast<std::chrono::seconds>(
               expected.time_since_epoch()
           ).count();
}

int test_single_file_download_coordination() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "single";
    auto config = onedrive::config::Config::defaults();
    config.sync_directory = root;
    config.drive_id = "drive-id";
    config.filesystem_metadata =
        onedrive::config::FilesystemMetadataMode::database;
    FakeGraphClient graph_implementation;
    graph_implementation.lookup_item =
        remote_item("single", "Documents/single.txt");
    FakeItemStore item_implementation;
    onedrive::graph::GraphClient graph{
        onedrive::detail::borrowed_proxy,
        graph_implementation
    };
    onedrive::storage::ItemStore items{
        onedrive::detail::borrowed_proxy,
        item_implementation
    };
    std::ostringstream output;
    std::ostringstream error;
    const onedrive::cli::Console console{{}, output, error};

    auto dry_config = config;
    dry_config.dry_run = true;
    if (onedrive::sync::plan_single_file_download(
            dry_config,
            "Documents/single.txt",
            graph,
            console
        ) != 0 ||
        graph_implementation.download_count != 0 ||
        std::filesystem::exists(root / "Documents/single.txt")) {
        return fail("single-file dry run changed local state");
    }

    if (onedrive::sync::download_single_file(
            config,
            "Documents/single.txt",
            graph,
            items,
            console
        ) != 0 ||
        graph_implementation.download_count != 1 ||
        !std::filesystem::is_regular_file(
            root / "Documents/single.txt"
        ) ||
        !item_implementation.states.contains("single")) {
        return fail("single-file download did not reuse the safe transaction");
    }

    graph_implementation.lookup_item =
        remote_item("directory", "Documents");
    graph_implementation.lookup_item.directory = true;
    try {
        static_cast<void>(onedrive::sync::download_single_file(
            config,
            "Documents",
            graph,
            items,
            console
        ));
        return fail("single-file download accepted a directory");
    } catch (const std::runtime_error&) {
    }

    graph_implementation.lookup_item =
        remote_item("malware", "malware.exe");
    graph_implementation.lookup_item.malware = true;
    try {
        static_cast<void>(onedrive::sync::download_single_file(
            config,
            "malware.exe",
            graph,
            items,
            console
        ));
        return fail("single-file download accepted Graph malware");
    } catch (const std::runtime_error&) {
    }

    graph_implementation.lookup_item =
        remote_item("conflict", "conflict.txt");
    std::filesystem::create_directories(root);
    {
        std::ofstream local{root / "conflict.txt"};
        local << "user data";
    }
    try {
        static_cast<void>(onedrive::sync::download_single_file(
            config,
            "conflict.txt",
            graph,
            items,
            console
        ));
        return fail("single-file download overwrote an untracked local file");
    } catch (const onedrive::sync::detail::LocalModificationConflictError&) {
    }
    if (graph_implementation.download_count != 1) {
        return fail("rejected single-file target started a download");
    }

    auto backup_config = config;
    backup_config.local_conflict =
        onedrive::config::LocalConflictPolicy::backup;
    if (onedrive::sync::download_single_file(
            backup_config,
            "conflict.txt",
            graph,
            items,
            console
        ) != 0 ||
        graph_implementation.download_count != 2) {
        return fail("single-file safeBackup download did not succeed");
    }
    std::filesystem::path conflict_backup;
    for (const auto& entry : std::filesystem::directory_iterator{root}) {
        if (entry.path().filename().string().starts_with(
                "conflict.safeBackup-"
            )) {
            conflict_backup = entry.path();
        }
    }
    std::ifstream installed_input{root / "conflict.txt"};
    std::ifstream backup_input{conflict_backup};
    const std::string installed_contents{
        std::istreambuf_iterator<char>{installed_input},
        std::istreambuf_iterator<char>{}
    };
    const std::string backup_contents{
        std::istreambuf_iterator<char>{backup_input},
        std::istreambuf_iterator<char>{}
    };
    if (conflict_backup.empty() || installed_contents != "data" ||
        backup_contents != "user data" ||
        !output.str().contains("Preserved local conflict")) {
        return fail("single-file safeBackup did not preserve local content");
    }

    graph_implementation.lookup_item =
        remote_item("identical", "identical.txt");
    {
        std::ofstream local{root / "identical.txt", std::ios::binary};
        local << "data";
    }
    struct stat before {};
    if (::stat((root / "identical.txt").c_str(), &before) == -1) {
        return fail("content-identical fixture could not be inspected");
    }
    if (onedrive::sync::download_single_file(
            backup_config,
            "identical.txt",
            graph,
            items,
            console
        ) != 0) {
        return fail("content-identical local file was not adopted");
    }
    struct stat after {};
    if (::stat((root / "identical.txt").c_str(), &after) == -1 ||
        before.st_ino != after.st_ino) {
        return fail("content-identical local file inode was replaced");
    }
    for (const auto& entry : std::filesystem::directory_iterator{root}) {
        if (entry.path().filename().string().starts_with(
                "identical.safeBackup-"
            )) {
            return fail("content-identical local file created a safeBackup");
        }
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    namespace detail = onedrive::sync::detail;

    if (const int result = test_single_file_download_coordination();
        result != EXIT_SUCCESS) {
        return result;
    }

    TemporaryDirectory temporary;
    const auto root = temporary.path() / "files";
    std::filesystem::create_directories(root);
    const auto metadata = detail::FilesystemMetadata::detect(
        onedrive::config::FilesystemMetadataMode::database,
        root
    );
    const detail::SafeSyncRoot safe_root{root};
    if (safe_root.remove(root / "missing.txt", false)) {
        return fail("safe removal reported a missing file as removed");
    }
    const auto removable_file = root / "remove.txt";
    {
        std::ofstream output{removable_file, std::ios::binary};
        output << "remove";
    }
    if (!safe_root.remove(removable_file, false) ||
        std::filesystem::exists(removable_file)) {
        return fail("safe removal did not remove a regular file");
    }
    const auto removable_directory = root / "remove-directory";
    std::filesystem::create_directory(removable_directory);
    if (!safe_root.remove(removable_directory, true) ||
        std::filesystem::exists(removable_directory)) {
        return fail("safe removal did not remove an empty directory");
    }
    const auto nonempty_directory = root / "nonempty-directory";
    std::filesystem::create_directory(nonempty_directory);
    {
        std::ofstream output{nonempty_directory / "local.txt"};
        output << "local";
    }
    try {
        static_cast<void>(safe_root.remove(nonempty_directory, true));
        return fail("safe removal accepted a non-empty directory");
    } catch (const detail::SafePathConflictError&) {
    }
    const auto removal_symlink = root / "remove-link";
    std::filesystem::create_symlink("missing-target", removal_symlink);
    try {
        static_cast<void>(safe_root.remove(removal_symlink, false));
        return fail("safe removal accepted a symbolic link");
    } catch (const detail::SafePathConflictError&) {
    }
    const auto local_source = root / "local.txt";
    {
        std::ofstream output{local_source, std::ios::binary};
        output << "local";
    }
    if (::chmod(local_source.c_str(), S_IRUSR | S_IWUSR | S_IRGRP) == -1) {
        return fail("safeBackup permission fixture could not be prepared");
    }
    const auto local_backup = detail::preserve_safe_backup(
        safe_root,
        local_source,
        detail::capture_local_file_baseline(local_source)
    );
    struct stat backup_status {};
    if (!local_backup.path.filename().string().starts_with(
            "local.safeBackup-"
        ) ||
        detail::content_fingerprint(local_backup.path) !=
            detail::content_fingerprint(local_source) ||
        ::stat(local_backup.path.c_str(), &backup_status) == -1 ||
        (backup_status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) !=
            (S_IRUSR | S_IWUSR | S_IRGRP)) {
        return fail("safeBackup copy did not preserve content and permissions");
    }
    const auto stale_source = root / "stale.txt";
    {
        std::ofstream output{stale_source, std::ios::binary};
        output << "old";
    }
    const auto stale_baseline =
        detail::capture_local_file_baseline(stale_source);
    {
        std::ofstream output{stale_source, std::ios::binary};
        output << "changed";
    }
    try {
        static_cast<void>(detail::preserve_safe_backup(
            safe_root,
            stale_source,
            stale_baseline
        ));
        return fail("safeBackup accepted a stale local baseline");
    } catch (const detail::LocalModificationConflictError&) {
    }
    try {
        static_cast<void>(detail::preserve_safe_backup(
            safe_root,
            stale_source,
            {}
        ));
        return fail("safeBackup accepted an absent local baseline");
    } catch (const std::invalid_argument&) {
    }
    const auto long_source =
        root / (std::string(230, 'x') + ".txt");
    {
        std::ofstream output{long_source, std::ios::binary};
        output << "long";
    }
    const auto long_backup = detail::preserve_safe_backup(
        safe_root,
        long_source,
        detail::capture_local_file_baseline(long_source)
    );
    if (!long_backup.path.filename().string().starts_with(
            "onedrive-"
        )) {
        return fail("long safeBackup name did not use a bounded digest");
    }
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
        installed.local_size != 4 || installed.local_modified_ticks == 0 ||
        graph.last_expected_etag != installed_item.etag ||
        !has_remote_modified_time(destination)) {
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
        !std::filesystem::exists(recover_destination) ||
        !has_remote_modified_time(recover_destination)) {
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

    graph.send_checkpoint = false;
    graph.progress_updates = {0, 2, 2, 4};
    auto relaxed_item = remote_item("relaxed", "protected.heic");
    relaxed_item.size = 2;
    relaxed_item.content_hash = onedrive::FileHash{
        .algorithm = onedrive::FileHashAlgorithm::sha256,
        .value =
            "0000000000000000000000000000000000000000000000000000000000000000",
    };
    relaxed_item.validate_content = false;
    const auto relaxed_destination = root / "protected.heic";
    const auto relaxed = detail::download_atomically(
        graph,
        items,
        relaxed_item,
        item_state(relaxed_item, relaxed_destination),
        relaxed_destination,
        metadata,
        space
    );
    if (!std::filesystem::exists(relaxed_destination) ||
        relaxed.local_size != 4 || relaxed.size != 4) {
        return fail(
            "relaxed download validation did not accept actual file metadata"
        );
    }

    auto zero_size_relaxed_item =
        remote_item("relaxed-zero", "protected-zero.heic");
    zero_size_relaxed_item.size = 0;
    zero_size_relaxed_item.validate_content = false;
    const auto zero_size_relaxed_destination =
        root / "protected-zero.heic";
    const auto zero_size_relaxed = detail::download_atomically(
        graph,
        items,
        zero_size_relaxed_item,
        item_state(
            zero_size_relaxed_item,
            zero_size_relaxed_destination
        ),
        zero_size_relaxed_destination,
        metadata,
        space
    );
    if (!std::filesystem::exists(zero_size_relaxed_destination) ||
        zero_size_relaxed.local_size != 4 ||
        zero_size_relaxed.size != 4) {
        return fail(
            "zero-size relaxed download did not reserve its actual bytes"
        );
    }

    auto overreported_relaxed_item =
        remote_item("relaxed-large", "protected-large.heic");
    overreported_relaxed_item.size = 100;
    overreported_relaxed_item.validate_content = false;
    const auto overreported_relaxed_destination =
        root / "protected-large.heic";
    detail::DownloadSpaceCoordinator actual_size_space{
        root,
        0,
        [](const std::filesystem::path&) {
            return std::uintmax_t{4};
        }
    };
    const auto overreported_relaxed = detail::download_atomically(
        graph,
        items,
        overreported_relaxed_item,
        item_state(
            overreported_relaxed_item,
            overreported_relaxed_destination
        ),
        overreported_relaxed_destination,
        metadata,
        actual_size_space
    );
    if (!std::filesystem::exists(overreported_relaxed_destination) ||
        overreported_relaxed.local_size != 4 ||
        overreported_relaxed.size != 4) {
        return fail(
            "relaxed download reserved an inaccurate remote file size"
        );
    }

    graph.progress_updates.clear();
    auto insufficient_relaxed_item =
        remote_item("relaxed-space", "protected-space.heic");
    insufficient_relaxed_item.size = 2;
    insufficient_relaxed_item.validate_content = false;
    const auto insufficient_relaxed_destination =
        root / "protected-space.heic";
    detail::DownloadSpaceCoordinator insufficient_relaxed_space{
        root,
        0,
        [](const std::filesystem::path&) {
            return std::uintmax_t{3};
        }
    };
    try {
        static_cast<void>(detail::download_atomically(
            graph,
            items,
            insufficient_relaxed_item,
            item_state(
                insufficient_relaxed_item,
                insufficient_relaxed_destination
            ),
            insufficient_relaxed_destination,
            metadata,
            insufficient_relaxed_space
        ));
        return fail("relaxed download exceeded dynamically available space");
    } catch (const std::runtime_error&) {
    }
    if (std::filesystem::exists(insufficient_relaxed_destination) ||
        items.partials.contains(insufficient_relaxed_item.id)) {
        return fail("failed relaxed space expansion retained download state");
    }
    auto released_space = insufficient_relaxed_space.acquire(3);
    if (released_space.remaining() != 3) {
        return fail("failed relaxed expansion did not release its reservation");
    }

    const auto relaxed_partial =
        root / ".protected-partial.heic.onedrive-partial-previous";
    {
        std::ofstream output{relaxed_partial, std::ios::binary};
        output << "da";
    }
    auto relaxed_partial_item =
        remote_item("relaxed-partial", "protected-partial.heic");
    relaxed_partial_item.validate_content = false;
    const auto relaxed_partial_destination =
        root / "protected-partial.heic";
    items.partials.emplace(
        relaxed_partial_item.id,
        onedrive::storage::PartialDownload{
            .item = item_state(
                relaxed_partial_item,
                relaxed_partial_destination
            ),
            .temporary_path = relaxed_partial,
            .completed_bytes = 2,
        }
    );
    static_cast<void>(detail::download_atomically(
        graph,
        items,
        relaxed_partial_item,
        item_state(relaxed_partial_item, relaxed_partial_destination),
        relaxed_partial_destination,
        metadata,
        space
    ));
    if (graph.last_initial_offset != 0 ||
        std::filesystem::exists(relaxed_partial) ||
        items.partials.contains(relaxed_partial_item.id)) {
        return fail("relaxed download reused a remote-size partial file");
    }

    graph.send_checkpoint = true;
    auto truncated_item = remote_item("truncated", "truncated.txt");
    truncated_item.content_hash = onedrive::FileHash{
        .algorithm = onedrive::FileHashAlgorithm::sha256,
        .value =
            "3A6EB0790F39AC87C94F3856B2DD2C5D110E6811602261A9A923D3BB23ADC8B7",
    };
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
                .last_modified = "2026-10-02T00:00:00Z",
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

    const auto replacing_destination = root / "replace-old.txt";
    const auto replacing_temporary = root / ".replace-old.partial";
    {
        std::ofstream destination_output{
            replacing_destination,
            std::ios::binary
        };
        destination_output << "old!";
        std::ofstream temporary_output{
            replacing_temporary,
            std::ios::binary
        };
        temporary_output << "data";
    }
    const auto old_baseline =
        detail::capture_local_file_baseline(replacing_destination);
    items.states.emplace(
        "replace-old",
        onedrive::storage::ItemState{
            .drive_id = "me",
            .remote_id = "replace-old",
            .name = "replace-old.txt",
            .etag = "old-etag",
            .remote_path = "replace-old.txt",
            .local_path = replacing_destination,
            .last_modified = "2026-10-01T00:00:00Z",
            .size = 4,
            .local_size = old_baseline.size,
            .local_modified_ticks = old_baseline.modified_ticks,
        }
    );
    items.pending.emplace(
        "replace-old",
        onedrive::storage::PendingDownload{
            .item = {
                .drive_id = "me",
                .remote_id = "replace-old",
                .name = "replace-old.txt",
                .etag = "new-etag",
                .remote_path = "replace-old.txt",
                .local_path = replacing_destination,
                .last_modified = "2026-10-02T00:00:00Z",
                .size = 4,
            },
            .temporary_path = replacing_temporary,
            .content_fingerprint =
                detail::content_fingerprint(replacing_temporary),
        }
    );
    detail::recover_pending_downloads(items, root, "me", metadata);
    std::ifstream replaced_input{replacing_destination, std::ios::binary};
    const std::string replaced_contents{
        std::istreambuf_iterator<char>{replaced_input},
        std::istreambuf_iterator<char>{}
    };
    if (replaced_contents != "data" ||
        items.pending.contains("replace-old")) {
        return fail(
            "journal recovery did not replace a trusted previous snapshot"
        );
    }

    const auto backed_up_destination = root / "backed-up.txt";
    const auto backed_up_temporary = root / ".backed-up.partial";
    const auto backup_path =
        root / "backed-up.safeBackup-20261004T051000Z-0001.txt";
    {
        std::ofstream destination_output{
            backed_up_destination,
            std::ios::binary
        };
        destination_output << "user";
        std::ofstream backup_output{backup_path, std::ios::binary};
        backup_output << "user";
        std::ofstream temporary_output{
            backed_up_temporary,
            std::ios::binary
        };
        temporary_output << "data";
    }
    items.pending.emplace(
        "backed-up",
        onedrive::storage::PendingDownload{
            .item = {
                .drive_id = "me",
                .remote_id = "backed-up",
                .name = "backed-up.txt",
                .etag = "etag",
                .remote_path = "backed-up.txt",
                .local_path = backed_up_destination,
                .last_modified = "2026-10-02T00:00:00Z",
                .size = 4,
            },
            .temporary_path = backed_up_temporary,
            .content_fingerprint =
                detail::content_fingerprint(backed_up_temporary),
            .backup_path = backup_path,
            .backup_fingerprint =
                detail::content_fingerprint(backup_path),
        }
    );
    detail::recover_pending_downloads(items, root, "me", metadata);
    std::ifstream recovered_input{
        backed_up_destination,
        std::ios::binary
    };
    const std::string recovered_contents{
        std::istreambuf_iterator<char>{recovered_input},
        std::istreambuf_iterator<char>{}
    };
    if (recovered_contents != "data" ||
        detail::content_fingerprint(backup_path) ==
            detail::content_fingerprint(backed_up_destination) ||
        items.pending.contains("backed-up")) {
        return fail("safeBackup recovery did not promote remote content");
    }

    const auto damaged_destination = root / "damaged-backup.txt";
    const auto damaged_temporary = root / ".damaged-backup.partial";
    const auto damaged_backup =
        root / "damaged-backup.safeBackup-20261004T051000Z-0001.txt";
    for (const auto& [path, contents] :
         std::vector<std::pair<std::filesystem::path, std::string>>{
             {damaged_destination, "user"},
             {damaged_backup, "user"},
             {damaged_temporary, "data"},
         }) {
        std::ofstream output{path, std::ios::binary};
        output << contents;
    }
    const auto expected_backup_fingerprint =
        detail::content_fingerprint(damaged_backup);
    {
        std::ofstream output{damaged_backup, std::ios::binary};
        output << "tampered";
    }
    items.pending.emplace(
        "damaged-backup",
        onedrive::storage::PendingDownload{
            .item = {
                .drive_id = "me",
                .remote_id = "damaged-backup",
                .name = "damaged-backup.txt",
                .etag = "etag",
                .remote_path = "damaged-backup.txt",
                .local_path = damaged_destination,
                .last_modified = "2026-10-02T00:00:00Z",
                .size = 4,
            },
            .temporary_path = damaged_temporary,
            .content_fingerprint =
                detail::content_fingerprint(damaged_temporary),
            .backup_path = damaged_backup,
            .backup_fingerprint = expected_backup_fingerprint,
        }
    );
    try {
        detail::recover_pending_downloads(items, root, "me", metadata);
        return fail("damaged safeBackup recovery was accepted");
    } catch (const std::runtime_error&) {
    }
    if (detail::content_fingerprint(damaged_destination) !=
        expected_backup_fingerprint) {
        return fail("damaged safeBackup recovery overwrote local content");
    }
    items.pending.erase("damaged-backup");

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
                .last_modified = "2026-10-02T00:00:00Z",
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
                .last_modified = "2026-10-02T00:00:00Z",
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
