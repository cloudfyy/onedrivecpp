#include "util/atomic_file.hpp"
#include "support/common.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <span>
#include <sys/stat.h>
#include <thread>

namespace {

using onedrive::test::fail;

template <typename Action>
bool throws_with(Action action, const std::string& expected) {
    try {
        action();
    } catch (const std::exception& error) {
        return std::string{error.what()}.contains(expected);
    }
    return false;
}

}  // namespace

int main() {
    const onedrive::test::TemporaryDirectory temporary;
    const auto destination = temporary.path() / "private";
    onedrive::util::write_file_atomically(
        destination,
        "first",
        S_IRUSR | S_IWUSR,
        "test file"
    );
    struct stat status {};
    if (onedrive::test::read_file(destination) != "first" ||
        ::stat(destination.c_str(), &status) == -1 ||
        (status.st_mode & 0777) != (S_IRUSR | S_IWUSR)) {
        return fail("atomic test file was not created securely");
    }

    onedrive::util::write_file_atomically(
        destination,
        "second",
        S_IRUSR | S_IWUSR,
        "test file"
    );
    if (onedrive::test::read_file(destination) != "second") {
        return fail("atomic test file was not replaced");
    }

    const std::array binary{
        std::byte{0xff}, std::byte{'a'}, std::byte{0},
        std::byte{'b'}, std::byte{0xff},
    };
    onedrive::util::write_file_atomically(
        destination,
        std::span{binary}.subspan(1, 3),
        S_IRUSR | S_IWUSR,
        "binary test file"
    );
    if (onedrive::test::read_file(destination) != std::string("a\0b", 3)) {
        return fail("atomic write did not respect binary span boundaries");
    }
    onedrive::util::write_file_atomically(
        destination,
        std::span<const std::byte>{},
        S_IRUSR | S_IWUSR,
        "empty test file"
    );
    if (!onedrive::test::read_file(destination).empty()) {
        return fail("empty atomic write retained previous contents");
    }

    std::jthread first{[&] {
        onedrive::util::write_file_atomically(
            destination,
            "concurrent-first",
            S_IRUSR | S_IWUSR,
            "test file"
        );
    }};
    std::jthread second{[&] {
        onedrive::util::write_file_atomically(
            destination,
            "concurrent-second",
            S_IRUSR | S_IWUSR,
            "test file"
        );
    }};
    first.join();
    second.join();
    const auto contents = onedrive::test::read_file(destination);
    if (contents != "concurrent-first" &&
        contents != "concurrent-second") {
        return fail("concurrent atomic test file write was corrupted");
    }
    for (const auto& entry :
         std::filesystem::directory_iterator{temporary.path()}) {
        if (entry.path() != destination) {
            return fail("atomic test file write left a temporary file");
        }
    }

    const auto real_parent = temporary.path() / "real-parent";
    const auto linked_parent = temporary.path() / "linked-parent";
    std::filesystem::create_directory(real_parent);
    std::filesystem::create_directory_symlink(real_parent, linked_parent);
    try {
        onedrive::util::write_file_atomically(
            linked_parent / "private",
            "data",
            S_IRUSR | S_IWUSR,
            "test file"
        );
        return fail("atomic test file followed a parent symlink");
    } catch (const std::runtime_error&) {
    }
    if (std::filesystem::exists(real_parent / "private")) {
        return fail("rejected atomic test file write modified its target");
    }

    if (!throws_with(
            [] {
                onedrive::util::write_file_atomically(
                    {},
                    "data",
                    S_IRUSR | S_IWUSR,
                    "test file"
                );
            },
            "requires a filename"
        )) {
        return fail("atomic write accepted an empty destination");
    }

    const auto excessive_name =
        temporary.path() / std::string(256, 'x');
    if (!throws_with(
            [&] {
                onedrive::util::write_file_atomically(
                    excessive_name,
                    "data",
                    S_IRUSR | S_IWUSR,
                    "test file"
                );
            },
            "cannot create test file"
        )) {
        return fail("atomic write accepted an excessive temporary filename");
    }

    const auto directory_target = temporary.path() / "directory-target";
    std::filesystem::create_directory(directory_target);
    if (!throws_with(
            [&] {
                onedrive::util::write_file_atomically(
                    directory_target,
                    "data",
                    S_IRUSR | S_IWUSR,
                    "test file"
                );
            },
            "cannot replace test file"
        )) {
        return fail("atomic write replaced a directory with a file");
    }
    for (const auto& entry :
         std::filesystem::directory_iterator{temporary.path()}) {
        if (entry.path().filename().string().contains(".tmp.")) {
            return fail("failed atomic write left a temporary file");
        }
    }
    return EXIT_SUCCESS;
}
