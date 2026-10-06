#include "util/atomic_file.hpp"

#include "onedrive/util/unique_file_descriptor.hpp"
#include "onedrive/util/path_security.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>

namespace onedrive::util {
namespace {

[[nodiscard]] std::string error_message(
    std::string_view action,
    std::string_view description,
    const std::filesystem::path& path,
    int error
) {
    return std::format(
        "cannot {} {} '{}': {}",
        action,
        description,
        path.string(),
        std::strerror(error)
    );
}

[[noreturn]] void throw_with_cleanup(
    std::string message,
    int directory,
    const std::string& temporary_name
) {
    if (::unlinkat(directory, temporary_name.c_str(), 0) == -1 &&
        errno != ENOENT) {
        message += std::format(
            "; additionally cannot remove temporary file '{}': {}",
            temporary_name,
            std::strerror(errno)
        );
    }
    throw std::runtime_error{message};
}

[[nodiscard]] UniqueFD create_temporary_file(
    int directory,
    const std::string& destination_name,
    mode_t mode,
    std::string& temporary_name,
    std::string_view description,
    const std::filesystem::path& destination
) {
    static std::atomic_uint64_t sequence_counter{0};
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        const auto sequence =
            sequence_counter.fetch_add(1, std::memory_order_relaxed);
        temporary_name = std::format(
            ".{}.tmp.{}.{}",
            destination_name,
            ::getpid(),
            sequence
        );
        const int descriptor = ::openat(
            directory,
            temporary_name.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            mode
        );
        if (descriptor != -1) {
            return UniqueFD{descriptor};
        }
        if (errno != EEXIST) {
            throw std::runtime_error{error_message(
                "create",
                description,
                destination,
                errno
            )};
        }
    }
    throw std::runtime_error{
        "cannot create " + std::string{description} + " '" +
        destination.string() + "': too many temporary file collisions"
    };
}

}  // namespace

void write_file_atomically(
    const std::filesystem::path& destination,
    std::span<const std::byte> contents,
    mode_t mode,
    std::string_view description
) {
    const auto parent = destination.has_parent_path() ?
                            destination.parent_path() :
                            std::filesystem::path{"."};
    const auto filename = destination.filename().string();
    if (filename.empty()) {
        throw std::invalid_argument(
            "atomic file destination requires a filename"
        );
    }

    UniqueFD directory{open_path_no_symlinks(
        parent,
        O_RDONLY | O_DIRECTORY
    )};

    std::string temporary_name;
    auto temporary = create_temporary_file(
        directory.get(),
        filename,
        mode,
        temporary_name,
        description,
        destination
    );
    if (::fchmod(temporary.get(), mode) == -1) {
        const int error = errno;
        temporary.reset();
        throw_with_cleanup(
            error_message(
                "secure",
                description,
                destination,
                error
            ),
            directory.get(),
            temporary_name
        );
    }

    std::size_t written = 0;
    while (written < contents.size()) {
        const auto count = ::write(
            temporary.get(),
            contents.data() + written,
            contents.size() - written
        );
        if (count == -1 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            const int error = errno;
            temporary.reset();
            throw_with_cleanup(
                error_message(
                    "write",
                    description,
                    destination,
                    error
                ),
                directory.get(),
                temporary_name
            );
        }
        written += static_cast<std::size_t>(count);
    }

    if (::fsync(temporary.get()) == -1) {
        const int error = errno;
        temporary.reset();
        throw_with_cleanup(
            error_message(
                "flush",
                description,
                destination,
                error
            ),
            directory.get(),
            temporary_name
        );
    }
    if (const auto error = temporary.close(); error) {
        throw_with_cleanup(
            error_message(
                "close",
                description,
                destination,
                error.value()
            ),
            directory.get(),
            temporary_name
        );
    }

    if (::renameat(
            directory.get(),
            temporary_name.c_str(),
            directory.get(),
            filename.c_str()
        ) == -1) {
        throw_with_cleanup(
            error_message(
                "replace",
                description,
                destination,
                errno
            ),
            directory.get(),
            temporary_name
        );
    }
    if (::fsync(directory.get()) == -1) {
        throw std::runtime_error{error_message(
            "flush parent directory for",
            description,
            destination,
            errno
        )};
    }
}

}  // namespace onedrive::util
