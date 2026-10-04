#include "filesystem_metadata.hpp"
#include "local_filesystem.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

using onedrive::test::TemporaryDirectory;

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

bool rejects_path_with(
    const std::filesystem::path& root,
    const std::string& path,
    std::string_view expected
) {
    try {
        static_cast<void>(
            onedrive::sync::detail::local_path_for(root, path)
        );
    } catch (const std::runtime_error& error) {
        return std::string_view{error.what()}.contains(expected);
    }
    return false;
}

}  // namespace

int main() {
    namespace detail = onedrive::sync::detail;

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
    if (!rejects_path_with(
            root,
            excessive_path,
            "resulting local path"
        )) {
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
    detail::apply_remote_modified_time(
        file,
        "2026-10-02T03:04:05.123456789Z"
    );
    struct stat remote_status {};
    if (::stat(file.c_str(), &remote_status) == -1) {
        return fail("cannot inspect applied remote modification time");
    }
    const auto expected_remote_time =
        std::chrono::sys_days{
            std::chrono::year{2026} / std::chrono::October / 2
        } +
        std::chrono::hours{3} +
        std::chrono::minutes{4} +
        std::chrono::seconds{5};
    if (remote_status.st_mtim.tv_sec !=
            std::chrono::duration_cast<std::chrono::seconds>(
                expected_remote_time.time_since_epoch()
            ).count() ||
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
            created_during_download,
            missing_baseline
        )) {
        return fail("file created after baseline was not detected");
    }
    try {
        static_cast<void>(
            detail::content_fingerprint(root / "missing.txt")
        );
        return fail("missing fingerprint input was accepted");
    } catch (const std::runtime_error&) {
    }

    const auto nested = root / "nested" / "directory";
    detail::ensure_directory_tree(root, nested);
    struct stat nested_status {};
    if (!std::filesystem::is_directory(nested) ||
        ::stat(nested.c_str(), &nested_status) == -1 ||
        (nested_status.st_mode & 0777) != 0700) {
        return fail("private directory tree was not created securely");
    }
    const auto conflicting_file = root / "not-a-directory";
    {
        std::ofstream output{conflicting_file};
        output << "data";
    }
    try {
        detail::ensure_directory_tree(
            root,
            conflicting_file / "directory"
        );
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

    const auto metadata = detail::FilesystemMetadata::detect(
        onedrive::config::FilesystemMetadataMode::database,
        root
    );
    if (metadata.uses_xattrs()) {
        return fail("database metadata mode enabled xattrs");
    }
    return EXIT_SUCCESS;
}
