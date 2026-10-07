#include "sync/filesystem/operations.hpp"
#include "sync/filesystem/metadata.hpp"
#include "onedrive/util/path_security.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include "support/common.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using onedrive::test::fail;
using onedrive::test::TemporaryDirectory;

bool rejects_path_with(
    const std::filesystem::path& root,
    const std::string& path,
    std::string_view expected
) {
    try {
        static_cast<void>(onedrive::sync::detail::local_path_for(root, path));
    } catch (const std::runtime_error& error) {
        return std::string_view{error.what()}.contains(expected);
    }
    return false;
}

} // namespace

int main() {
    namespace detail = onedrive::sync::detail;

    if (detail::persisted_file_size(
            static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max()
            )
        ) != std::numeric_limits<std::int64_t>::max()) {
        return fail("maximum persistent file size was not preserved");
    }
    try {
        static_cast<void>(detail::persisted_file_size(
            std::numeric_limits<std::uintmax_t>::max()
        ));
        return fail("unrepresentable persistent file size was accepted");
    } catch (const std::runtime_error&) {
    }

    TemporaryDirectory temporary;
    const auto root = temporary.path() / "root";
    std::filesystem::create_directories(root);
    if (detail::local_path_for(root, "Documents/file.txt") !=
        root / "Documents/file.txt") {
        return fail("safe remote path was not mapped below the sync root");
    }
    const std::vector<std::pair<std::string, std::string_view>> invalid_paths{
        {"", "the path is empty"},
        {"/absolute", "absolute paths are not allowed"},
        {"../escape", "reserved component '..'"},
        {"a/../escape", "reserved component '..'"},
        {"a//file", "empty path components"},
        {"directory/", "empty path components"},
        {std::string{"bad\0name", 8}, "bad\\x00name"},
        {"line\nbreak", "control byte 0x0A"},
        {std::string(256, 'x'), "256 bytes"},
    };
    for (const auto& [path, expected] : invalid_paths) {
        if (!rejects_path_with(root, path, expected)) {
            return fail(
                "invalid remote path did not report the expected reason: " +
                std::string{expected}
            );
        }
    }
    std::string excessive_path;
    for (int index = 0; index < 20; ++index) {
        if (!excessive_path.empty()) {
            excessive_path += '/';
        }
        excessive_path += std::string(240, 'a' + index % 26);
    }
    if (!rejects_path_with(root, excessive_path, "resulting local path")) {
        return fail("excessive complete path length was accepted");
    }

    const auto file = root / "data.txt";
    {
        std::ofstream output{file, std::ios::binary};
        output << "data";
    }
    if (detail::content_fingerprint(file) !=
        "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602261a9a923d3bb23adc8b7") {
        return fail("SHA-256 content fingerprint was incorrect");
    }
    const onedrive::storage::ItemState snapshot{
        .local_size = 4,
        .local_modified_ticks = detail::modified_ticks(file),
    };
    if (!detail::local_snapshot_matches(snapshot, file)) {
        return fail("unchanged local snapshot did not match");
    }
    auto changed_snapshot = snapshot;
    changed_snapshot.local_size = 5;
    if (detail::local_snapshot_matches(changed_snapshot, file)) {
        return fail("changed local snapshot incorrectly matched");
    }
    const auto baseline = detail::capture_local_file_baseline(file);
    if (!baseline.existed ||
        !detail::local_file_matches_baseline(file, baseline)) {
        return fail("unchanged local file baseline did not match");
    }
    {
        std::ofstream output{file, std::ios::binary};
        output << "edit";
    }
    if (detail::local_file_matches_baseline(file, baseline)) {
        return fail("same-size local content change was not detected");
    }
    detail::apply_remote_modified_time(file, "2026-10-02T03:04:05.123456789Z");
    struct stat remote_status{};
    if (::stat(file.c_str(), &remote_status) == -1) {
        return fail("cannot inspect applied remote modification time");
    }
    const auto expected_remote_time =
        std::chrono::sys_days{
            std::chrono::year{2026} / std::chrono::October / 2
        } +
        std::chrono::hours{3} + std::chrono::minutes{4} +
        std::chrono::seconds{5};
    if (remote_status.st_mtim.tv_sec !=
            std::chrono::duration_cast<std::chrono::seconds>(
                expected_remote_time.time_since_epoch()
            )
                .count() ||
        remote_status.st_mtim.tv_nsec != 123'456'789) {
        return fail("remote modification time was not applied precisely");
    }
    try {
        detail::apply_remote_modified_time(file, "2026-02-30T00:00:00Z");
        return fail("invalid remote modification time was accepted");
    } catch (const std::runtime_error&) {
    }
    const auto created_during_download = root / "created-later.txt";
    const auto missing_baseline =
        detail::capture_local_file_baseline(created_during_download);
    {
        std::ofstream output{created_during_download};
        output << "user data";
    }
    if (detail::local_file_matches_baseline(
            created_during_download, missing_baseline
        )) {
        return fail("file created after baseline was not detected");
    }
    try {
        static_cast<void>(detail::content_fingerprint(root / "missing.txt"));
        return fail("missing fingerprint input was accepted");
    } catch (const std::runtime_error&) {
    }

    const auto nested = root / "nested" / "directory";
    detail::ensure_directory_tree(root, nested);
    struct stat nested_status{};
    if (!std::filesystem::is_directory(nested) ||
        ::stat(nested.c_str(), &nested_status) == -1 ||
        (nested_status.st_mode & 0777) != 0700) {
        return fail("private directory tree was not created securely");
    }
    const auto attack_outside = temporary.path() / "attack-outside";
    std::filesystem::create_directory(attack_outside);
    const auto redirected = root / "redirected";
    std::filesystem::create_directory_symlink(attack_outside, redirected);
    detail::SafeSyncRoot safe_root{root};
    detail::SafeSyncRoot moved_root{std::move(safe_root)};
    detail::SafeSyncRoot assigned_root{root};
    assigned_root = std::move(moved_root);
    try {
        assigned_root.ensure_directory_tree(
            redirected / "directory",
            onedrive::config::SyncPermissionsMode::private_access
        );
        return fail("safe root followed a directory symlink");
    } catch (const std::runtime_error&) {
    }
    try {
        static_cast<void>(assigned_root.open(
            redirected / "opened.txt",
            O_WRONLY | O_CREAT | O_EXCL,
            S_IRUSR | S_IWUSR
        ));
        return fail("safe root opened a file through a directory symlink");
    } catch (const std::runtime_error&) {
    }
    try {
        static_cast<void>(onedrive::util::open_path_no_symlinks(
            redirected / "shared-open.txt",
            O_WRONLY | O_CREAT | O_EXCL,
            S_IRUSR | S_IWUSR
        ));
        return fail("shared safe open followed a directory symlink");
    } catch (const std::runtime_error&) {
    }
    const auto umask_directory = root / "umask-directory";
    const mode_t original_umask = ::umask(0022);
    const auto umask_file = root / "umask-file.txt";
    static_cast<void>(onedrive::util::open_path_no_symlinks(
        umask_file,
        O_WRONLY | O_CREAT | O_EXCL,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH
    ));
    assigned_root.ensure_directory_tree(
        umask_directory, onedrive::config::SyncPermissionsMode::umask
    );
    ::umask(original_umask);
    struct stat umask_status{};
    struct stat umask_file_status{};
    if (::stat(umask_file.c_str(), &umask_file_status) == -1 ||
        (umask_file_status.st_mode & 0777) != 0644 ||
        ::stat(umask_directory.c_str(), &umask_status) == -1 ||
        (umask_status.st_mode & 0777) != 0755) {
        return fail("file or directory mode did not follow the process umask");
    }
    const auto rename_source = root / "rename-source.txt";
    {
        std::ofstream output{rename_source};
        output << "data";
    }
    try {
        assigned_root.rename(rename_source, redirected / "renamed.txt");
        return fail("safe root renamed a file through a directory symlink");
    } catch (const std::runtime_error&) {
    }
    if (!std::filesystem::exists(rename_source) ||
        std::filesystem::exists(attack_outside / "directory") ||
        std::filesystem::exists(attack_outside / "opened.txt") ||
        std::filesystem::exists(attack_outside / "shared-open.txt") ||
        std::filesystem::exists(attack_outside / "renamed.txt")) {
        return fail("safe root modified data outside the synchronization root");
    }

    if (assigned_root.path() != std::filesystem::absolute(root) ||
        assigned_root.relative_path(root / "child") != "child") {
        return fail("safe root did not preserve or relativize its path");
    }
    try {
        static_cast<void>(
            assigned_root.relative_path(temporary.path() / "outside.txt")
        );
        return fail("safe root accepted a path outside its root");
    } catch (const std::runtime_error&) {
    }
    try {
        detail::SafeSyncRoot invalid_root{rename_source};
        return fail("regular file was accepted as a safe root");
    } catch (const std::runtime_error&) {
    }

    {
        auto descriptor = assigned_root.open(rename_source, O_RDONLY);
        if (!descriptor) {
            return fail("safe root did not open an existing file");
        }
    }
    {
        auto descriptor = assigned_root.open_directory(root);
        if (!descriptor) {
            return fail("safe root did not open its root directory");
        }
    }
    try {
        static_cast<void>(assigned_root.open_directory(rename_source));
        return fail("safe root opened a regular file as a directory");
    } catch (const std::runtime_error&) {
    }

    const auto rename_destination = root / "rename-destination.txt";
    assigned_root.rename(rename_source, rename_destination);
    if (std::filesystem::exists(rename_source) ||
        !std::filesystem::exists(rename_destination)) {
        return fail("safe root rename did not move the file");
    }
    const auto invalid_rename_source = root / "invalid-rename-source.txt";
    const auto invalid_rename_destination = root / "invalid-rename-destination";
    {
        std::ofstream output{invalid_rename_source};
        output << "data";
    }
    std::filesystem::create_directory(invalid_rename_destination);
    try {
        assigned_root.rename(invalid_rename_source, invalid_rename_destination);
        return fail("safe root renamed a file over a directory");
    } catch (const std::runtime_error&) {
    }
    const auto no_replace_source = root / "no-replace-source.txt";
    const auto no_replace_destination = root / "no-replace-destination.txt";
    {
        std::ofstream output{no_replace_source};
        output << "source";
    }
    if (!assigned_root.rename_no_replace(
            no_replace_source, no_replace_destination
        )) {
        return fail("safe no-replace rename did not move an available file");
    }
    {
        std::ofstream output{no_replace_source};
        output << "replacement";
    }
    if (assigned_root.rename_no_replace(
            no_replace_source, no_replace_destination
        )) {
        return fail("safe no-replace rename overwrote an existing file");
    }

    const auto identity = assigned_root.identity(
        no_replace_source, detail::FilesystemItemKind::file
    );
    const auto root_identity =
        assigned_root.identity(root, detail::FilesystemItemKind::directory);
    if (identity.device == 0 || identity.inode == 0 ||
        root_identity.device == 0 || root_identity.inode == 0) {
        return fail("safe root returned an empty filesystem identity");
    }
    try {
        static_cast<void>(assigned_root.identity(
            no_replace_source, detail::FilesystemItemKind::directory
        ));
        return fail("safe root accepted a file as a directory identity");
    } catch (const detail::SafePathConflictError&) {
    }
    try {
        static_cast<void>(assigned_root.identity(
            root / "missing-identity", detail::FilesystemItemKind::file
        ));
        return fail("safe root returned an identity for a missing file");
    } catch (const std::runtime_error&) {
    }

    const auto nonempty_directory = root / "nonempty";
    std::filesystem::create_directory(nonempty_directory);
    {
        std::ofstream output{nonempty_directory / "child.txt"};
        output << "child";
    }
    try {
        static_cast<void>(assigned_root.remove(
            nonempty_directory, detail::FilesystemItemKind::directory
        ));
        return fail("safe root removed a nonempty directory");
    } catch (const detail::SafePathConflictError&) {
    }
    if (!assigned_root.remove(
            no_replace_source, detail::FilesystemItemKind::file
        ) ||
        assigned_root.remove(
            no_replace_source, detail::FilesystemItemKind::file
        )) {
        return fail("safe root file removal returned the wrong result");
    }
    try {
        static_cast<void>(assigned_root.remove(
            no_replace_destination, detail::FilesystemItemKind::directory
        ));
        return fail("safe root removed a file as a directory");
    } catch (const detail::SafePathConflictError&) {
    }
    std::filesystem::remove(nonempty_directory / "child.txt");
    if (!assigned_root.remove(
            nonempty_directory, detail::FilesystemItemKind::directory
        )) {
        return fail("safe root did not remove an empty directory");
    }
    assigned_root.fsync_directory(root);

    const auto conflicting_file = root / "not-a-directory";
    {
        std::ofstream output{conflicting_file};
        output << "data";
    }
    try {
        detail::ensure_directory_tree(root, conflicting_file / "directory");
        return fail("file was accepted as a directory component");
    } catch (const std::runtime_error&) {
    }
    const auto outside = temporary.path() / "outside";
    std::filesystem::create_directories(outside);
    std::filesystem::create_directory_symlink(outside, root / "linked");
    try {
        detail::ensure_directory_tree(root, root / "linked" / "directory");
        return fail("symbolic-link directory escape was accepted");
    } catch (const std::runtime_error&) {
    }

    const auto removable = root / "removable.txt";
    {
        std::ofstream output{removable};
        output << "data";
    }
    if (!detail::remove_no_symlinks(
            removable, detail::MissingPathPolicy::report
        ) ||
        std::filesystem::exists(removable)) {
        return fail("safe removal did not remove an existing file");
    }
    if (detail::remove_no_symlinks(
            removable, detail::MissingPathPolicy::ignore
        )) {
        return fail("ignored missing removal reported a removed file");
    }
    try {
        static_cast<void>(detail::remove_no_symlinks(
            removable, detail::MissingPathPolicy::report
        ));
        return fail("reported missing removal did not fail");
    } catch (const std::runtime_error&) {
    }

    const auto metadata = detail::FilesystemMetadata::detect(
        onedrive::config::FilesystemMetadataMode::database, root
    );
    if (metadata.uses_xattrs()) {
        return fail("database metadata mode enabled xattrs");
    }
    return EXIT_SUCCESS;
}
