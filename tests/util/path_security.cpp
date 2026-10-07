#include "onedrive/util/path_security.hpp"
#include "support/common.hpp"

#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>

namespace {

using onedrive::test::fail;
using onedrive::test::TemporaryDirectory;

template <typename Function>
[[nodiscard]] bool throws_runtime_error(Function&& function) {
    try {
        function();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    TemporaryDirectory temporary;
    std::filesystem::permissions(
        temporary.path(), std::filesystem::perms::all
    );
    auto directory = onedrive::util::open_path_no_symlinks(
        temporary.path(), O_RDONLY | O_DIRECTORY
    );
    const auto directory_status =
        onedrive::util::secure_owned_directory(
            directory.get(),
            temporary.path(),
            S_IRWXU,
            "test directory"
        );
    if (!S_ISDIR(directory_status.st_mode) ||
        (directory_status.st_mode & 07777) != S_IRWXU) {
        return fail("owned directory permissions were not secured");
    }

    const auto file_path = temporary.path() / "private";
    onedrive::test::write_file(file_path, "fixture");
    std::filesystem::permissions(file_path, std::filesystem::perms::all);
    auto file = onedrive::util::open_path_no_symlinks(file_path, O_RDONLY);
    const auto file_status =
        onedrive::util::secure_owned_regular_file(
            file.get(),
            file_path,
            S_IRUSR | S_IWUSR,
            "test file"
        );
    if (!S_ISREG(file_status.st_mode) ||
        (file_status.st_mode & 07777) != (S_IRUSR | S_IWUSR)) {
        return fail("owned regular-file permissions were not secured");
    }

    if (!throws_runtime_error([&] {
            static_cast<void>(onedrive::util::inspect_owned_directory(
                file.get(), file_path, "test directory"
            ));
        }) ||
        !throws_runtime_error([&] {
            static_cast<void>(
                onedrive::util::inspect_owned_regular_file(
                    directory.get(),
                    temporary.path(),
                    "test file"
                )
            );
        })) {
        return fail("owned path type mismatch was accepted");
    }

    const auto alias = temporary.path() / "private.alias";
    std::filesystem::create_hard_link(file_path, alias);
    if (!throws_runtime_error([&] {
            static_cast<void>(
                onedrive::util::inspect_owned_regular_file(
                    file.get(), file_path, "test file"
                )
            );
        })) {
        return fail("hard-linked owned file was accepted");
    }
    return EXIT_SUCCESS;
}
