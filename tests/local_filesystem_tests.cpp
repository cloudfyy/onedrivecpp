#include "filesystem_metadata.hpp"
#include "local_filesystem.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_{
              std::filesystem::temp_directory_path() /
              ("onedrive-cpp-filesystem-" +
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

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
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
    for (const std::string path : {"", "/absolute", "../escape", "a/../escape"}) {
        try {
            static_cast<void>(detail::local_path_for(root, path));
            return fail("unsafe remote path was accepted");
        } catch (const std::runtime_error&) {
        }
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

    const auto nested = root / "nested" / "directory";
    detail::ensure_directory_tree(root, nested);
    if (!std::filesystem::is_directory(nested)) {
        return fail("safe directory tree was not created");
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
