#include "onedrive/util/path_security.hpp"
#include "support/common.hpp"

#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <cerrno>
#include <unistd.h>

namespace {

using onedrive::test::fail;
using onedrive::test::TemporaryDirectory;
bool fail_close = false;

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

extern "C" int __real_close(int);
extern "C" int __wrap_close(int descriptor) {
    const int result = __real_close(descriptor);
    if (fail_close) {
        fail_close = false;
        errno = EIO;
        return -1;
    }
    return result;
}

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

    const auto shared_path = temporary.path() / "shared";
    std::filesystem::create_directory(shared_path);
    std::filesystem::permissions(shared_path, std::filesystem::perms::all);
    onedrive::util::secure_owned_directory(
        shared_path, S_IRWXU, "shared directory"
    );
    onedrive::util::secure_owned_directory(
        shared_path, S_IRWXU, "shared directory"
    );
    if ((std::filesystem::status(shared_path).permissions() &
         std::filesystem::perms::mask) != std::filesystem::perms::owner_all) {
        return fail(
            "shared directory helper did not apply private permissions"
        );
    }
    const auto missing = shared_path / "missing";
    if (!throws_runtime_error([&] {
            onedrive::util::secure_owned_directory(
                missing, S_IRWXU, "missing directory"
            );
        }) ||
        std::filesystem::exists(missing)) {
        return fail(
            "shared directory helper unexpectedly created a missing directory"
        );
    }
    const auto external = temporary.path() / "external";
    std::filesystem::create_directories(external / "nested");
    std::filesystem::permissions(external, std::filesystem::perms::all);
    std::filesystem::permissions(
        external / "nested", std::filesystem::perms::all
    );
    const auto link = temporary.path() / "link";
    std::filesystem::create_directory_symlink(external, link);
    for (const auto& unsafe : {link, link / "nested"}) {
        if (!throws_runtime_error([&] {
                onedrive::util::secure_owned_directory(
                    unsafe, S_IRWXU, "linked directory"
                );
            })) {
            return fail("shared directory helper followed a symlink");
        }
    }
    if (std::filesystem::status(external).permissions() !=
            std::filesystem::perms::all ||
        std::filesystem::status(external / "nested").permissions() !=
            std::filesystem::perms::all) {
        return fail("shared directory helper modified a symlink target");
    }
    fail_close = true;
    if (!onedrive::test::throws_with<std::runtime_error>(
            [&] {
                onedrive::util::secure_owned_directory(
                    shared_path, S_IRWXU, "shared directory"
                );
            },
            "cannot close shared directory"
        ) ||
        fail_close) {
        return fail("shared directory helper did not report a close failure");
    }

    const auto file_path = temporary.path() / "private";
    onedrive::test::write_file(file_path, "fixture");
    std::filesystem::permissions(file_path, std::filesystem::perms::all);
    if (!throws_runtime_error([&] {
            onedrive::util::secure_owned_directory(
                file_path, S_IRWXU, "file as directory"
            );
        }) ||
        std::filesystem::status(file_path).permissions() !=
            std::filesystem::perms::all) {
        return fail(
            "shared directory helper accepted or modified a regular file"
        );
    }
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
