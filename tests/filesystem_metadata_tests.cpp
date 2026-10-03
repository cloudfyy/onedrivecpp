#include "filesystem_metadata.hpp"
#include "local_filesystem.hpp"
#include "test_support.hpp"

#include <sys/xattr.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using onedrive::test::TemporaryDirectory;

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

std::string read_xattr(
    const std::filesystem::path& path,
    const char* name
) {
    std::string value(128, '\0');
    const auto size = ::getxattr(
        path.c_str(),
        name,
        value.data(),
        value.size()
    );
    if (size < 0) {
        throw std::runtime_error{"cannot read test xattr"};
    }
    value.resize(static_cast<std::size_t>(size));
    return value;
}

}  // namespace

int main() {
    namespace detail = onedrive::sync::detail;
    using onedrive::config::FilesystemMetadataMode;

    if (!detail::xattr_error_is_unavailable(ENOTSUP) ||
        !detail::xattr_error_is_unavailable(EOPNOTSUPP) ||
        !detail::xattr_error_is_unavailable(EPERM) ||
        !detail::xattr_error_is_unavailable(EACCES) ||
        !detail::xattr_error_is_unavailable(ENODATA) ||
        detail::xattr_error_is_unavailable(EIO)) {
        return fail("xattr availability errors were classified incorrectly");
    }

    if (detail::FilesystemMetadata::from_detected_support(
            FilesystemMetadataMode::database,
            true
        ).uses_xattrs() ||
        detail::FilesystemMetadata::from_detected_support(
            FilesystemMetadataMode::automatic,
            false
        ).uses_xattrs() ||
        !detail::FilesystemMetadata::from_detected_support(
            FilesystemMetadataMode::automatic,
            true
        ).uses_xattrs() ||
        !detail::FilesystemMetadata::from_detected_support(
            FilesystemMetadataMode::xattr,
            true
        ).uses_xattrs()) {
        return fail("detected xattr support selected the wrong strategy");
    }
    try {
        static_cast<void>(
            detail::FilesystemMetadata::from_detected_support(
                FilesystemMetadataMode::xattr,
                false
            )
        );
        return fail("required xattr mode accepted unavailable xattrs");
    } catch (const std::runtime_error&) {
    }

    TemporaryDirectory temporary;
    const auto root = temporary.path() / "root";
    std::filesystem::create_directories(root);
    const auto automatic = detail::FilesystemMetadata::detect(
        FilesystemMetadataMode::automatic,
        root
    );
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().filename().string().starts_with(
                ".onedrive-cpp-xattr-probe-"
            )) {
            return fail("xattr capability probe file was not removed");
        }
    }

    const auto file = root / "metadata.txt";
    {
        std::ofstream output{file};
        output << "data";
    }
    if (automatic.uses_xattrs()) {
        const onedrive::graph::RemoteItem item{
            .id = "remote-id",
            .name = "metadata.txt",
            .etag = "remote-etag",
        };
        detail::apply_remote_modified_time(
            file,
            "2026-10-02T03:04:05.123456789Z"
        );
        automatic.write_remote_identity(item, file);
        if (read_xattr(file, "user.onedrive.remote_id") != item.id ||
            read_xattr(file, "user.onedrive.etag") != item.etag ||
            read_xattr(file, "user.onedrive.local_modified_ticks") !=
                std::to_string(detail::modified_ticks(file))) {
            return fail("remote identity xattrs did not round-trip");
        }
        try {
            automatic.write_remote_identity(item, root / "missing.txt");
            return fail("xattr write failure was accepted");
        } catch (const std::runtime_error&) {
        }
    }

    const auto database = detail::FilesystemMetadata::detect(
        FilesystemMetadataMode::database,
        root / "does-not-need-to-exist"
    );
    if (database.uses_xattrs()) {
        return fail("database mode probed or enabled xattrs");
    }
    try {
        static_cast<void>(detail::FilesystemMetadata::detect(
            FilesystemMetadataMode::automatic,
            file
        ));
        return fail("xattr probe creation failure was accepted");
    } catch (const std::runtime_error&) {
    }
    return EXIT_SUCCESS;
}
