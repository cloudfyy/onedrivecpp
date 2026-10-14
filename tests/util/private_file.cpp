#include "util/private_file.hpp"
#include "support/common.hpp"
#include "onedrive/util/path_security.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using onedrive::test::fail;
using onedrive::test::throws_with;
using onedrive::util::PrivateFileRequirements;
using onedrive::util::read_private_file;

int test_size_boundaries() {
    onedrive::test::TemporaryDirectory temporary;
    const auto path = temporary.path() / "private";
    for (const std::size_t limit : {0U, 1U, 4096U, 65536U, 1048576U}) {
        const PrivateFileRequirements requirements{.maximum_size = limit};
        const std::string contents(limit, 'x');
        onedrive::test::write_file(path, contents);
        if (read_private_file(path, "test", requirements) != contents) {
            return fail("private file rejected or changed size-limit contents");
        }
        onedrive::test::write_file(path, contents + "x");
        if (!throws_with<std::runtime_error>(
                [&] {
                    static_cast<void>(
                        read_private_file(path, "test", requirements)
                    );
                },
                std::to_string(limit) + " byte size limit"
            )) {
            return fail("private file did not reject limit plus one byte");
        }
    }
    const std::string binary{"a\0b\n", 4};
    onedrive::test::write_file(path, binary);
    if (read_private_file(path, "test", {}) != binary) {
        return fail("private file reader did not preserve binary contents");
    }
    return EXIT_SUCCESS;
}

int test_permissions_and_links() {
    onedrive::test::TemporaryDirectory temporary;
    const auto path = temporary.path() / "private";
    onedrive::test::write_file(path, "secret");
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
    );
    PrivateFileRequirements requirements{
        .required_owner = ::geteuid(),
        .exact_permissions = 0600,
        .permission_requirement = "requires private permissions",
        .require_single_link = true,
    };
    if (read_private_file(path, "test", requirements) != "secret") {
        return fail("private owned file was rejected");
    }
    requirements.required_owner = ::geteuid() == 0 ? 1 : 0;
    if (!throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(
                    read_private_file(path, "test", requirements)
                );
            },
            "requires private permissions"
        )) {
        return fail("private file accepted an unexpected owner");
    }
    requirements.required_owner = ::geteuid();
    std::filesystem::permissions(path, std::filesystem::perms::owner_read);
    if (!throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(
                    read_private_file(path, "test", requirements)
                );
            },
            "requires private permissions"
        )) {
        return fail("private file ignored exact permissions");
    }
    requirements.exact_permissions.reset();
    requirements.forbidden_permissions = S_IRGRP;
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::group_read
    );
    if (!throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(
                    read_private_file(path, "test", requirements)
                );
            },
            "requires private permissions"
        )) {
        return fail("private file ignored forbidden permissions");
    }
    requirements.forbidden_permissions = 0;
    std::filesystem::create_hard_link(path, temporary.path() / "hard-link");
    if (!throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(
                    read_private_file(path, "test", requirements)
                );
            },
            "exactly one hard link"
        )) {
        return fail("private file accepted multiple links when forbidden");
    }
    requirements.require_single_link = false;
    if (read_private_file(path, "test", requirements) != "secret") {
        return fail("private file rejected allowed hard links");
    }
    return EXIT_SUCCESS;
}

int test_non_regular_paths() {
    onedrive::test::TemporaryDirectory temporary;
    const auto fifo = temporary.path() / "fifo";
    if (::mkfifo(fifo.c_str(), 0600) != 0) {
        return fail("cannot create private file FIFO fixture");
    }
    for (const auto& path : {temporary.path(), fifo}) {
        if (!throws_with<std::runtime_error>(
                [&] { static_cast<void>(read_private_file(path, "test", {})); },
                "not a regular file"
            )) {
            return fail("private file reader accepted a directory or FIFO");
        }
    }
    const auto file = temporary.path() / "file";
    const auto link = temporary.path() / "symlink";
    onedrive::test::write_file(file, "secret");
    std::filesystem::create_symlink(file, link);
    if (!throws_with<std::runtime_error>([&] {
            static_cast<void>(read_private_file(link, "test", {}));
        }) ||
        !throws_with<std::runtime_error>([&] {
            static_cast<void>(
                read_private_file(temporary.path() / "missing", "test", {})
            );
        })) {
        return fail("private file accepted a symlink or missing path");
    }
    return EXIT_SUCCESS;
}

int test_borrowed_descriptor() {
    onedrive::test::TemporaryDirectory temporary;
    const auto path = temporary.path() / "private";
    const std::string contents{"payload\0\n", 9};
    onedrive::test::write_file(path, contents);
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
    );
    auto descriptor = onedrive::util::open_path_no_symlinks(path, O_RDONLY);
    if (::flock(descriptor.get(), LOCK_EX | LOCK_NB) == -1 ||
        ::lseek(descriptor.get(), 2, SEEK_SET) != 2) {
        return fail("cannot prepare locked private file descriptor");
    }
    const PrivateFileRequirements requirements{
        .required_owner = ::geteuid(),
        .exact_permissions = 0600,
        .permission_requirement = "requires private permissions",
        .maximum_size = contents.size(),
        .require_single_link = true,
    };
    for (int sample = 0; sample < 2; ++sample) {
        if (read_private_file(descriptor.get(), path, "test", requirements) !=
                contents ||
            ::lseek(descriptor.get(), 0, SEEK_CUR) != 2) {
            return fail(
                "borrowed private-file read changed contents or file offset"
            );
        }
    }
    const auto moved = temporary.path() / "moved";
    std::filesystem::rename(path, moved);
    onedrive::test::write_file(path, "replacement");
    if (read_private_file(descriptor.get(), path, "test", requirements) !=
        contents) {
        return fail(
            "borrowed read reopened the path instead of reading the locked "
            "inode"
        );
    }
    const auto contender =
        onedrive::util::open_path_no_symlinks(moved, O_RDONLY);
    if (::flock(contender.get(), LOCK_EX | LOCK_NB) != -1 ||
        (errno != EWOULDBLOCK && errno != EAGAIN)) {
        return fail("borrowed private-file read closed its caller's lock");
    }
    auto too_small = requirements;
    too_small.maximum_size = contents.size() - 1;
    if (!throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(
                    read_private_file(descriptor.get(), path, "test", too_small)
                );
            },
            "byte size limit"
        ) ||
        !throws_with<std::runtime_error>(
            [&] { static_cast<void>(read_private_file(-1, path, "test", {})); },
            "cannot inspect"
        )) {
        return fail(
            "borrowed private-file read ignored size or descriptor errors"
        );
    }
    auto wrong_owner = requirements;
    wrong_owner.required_owner = ::geteuid() == 0 ? 1 : 0;
    if (!throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(read_private_file(
                    descriptor.get(), path, "test", wrong_owner
                ));
            },
            "requires private permissions"
        )) {
        return fail("borrowed private-file read ignored ownership validation");
    }
    descriptor.reset();
    if (::flock(contender.get(), LOCK_EX | LOCK_NB) == -1) {
        return fail(
            "caller could not release its private-file lock after reading"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    for (auto test :
         {test_size_boundaries,
          test_permissions_and_links,
          test_non_regular_paths,
          test_borrowed_descriptor}) {
        if (const int result = test(); result != EXIT_SUCCESS) {
            return result;
        }
    }
    return EXIT_SUCCESS;
}
