#include "local_filesystem.hpp"

#include <openssl/evp.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace onedrive::sync::detail {

std::filesystem::path local_path_for(
    const std::filesystem::path& sync_directory,
    const std::string& remote_path
) {
    const std::filesystem::path relative_path{remote_path};
    if (relative_path.empty() || relative_path.is_absolute()) {
        throw std::runtime_error("Microsoft Graph returned an invalid remote path");
    }
    for (const auto& component : relative_path) {
        if (component == "." || component == "..") {
            throw std::runtime_error(
                "Microsoft Graph returned an unsafe remote path"
            );
        }
    }
    return sync_directory / relative_path;
}

std::int64_t modified_ticks(const std::filesystem::path& path) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::filesystem::last_write_time(path).time_since_epoch()
    ).count();
}

bool local_snapshot_matches(
    const storage::ItemState& state,
    const std::filesystem::path& path
) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return false;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > static_cast<std::uintmax_t>(
                            std::numeric_limits<std::int64_t>::max()
                        )) {
        return false;
    }
    return static_cast<std::int64_t>(size) == state.local_size &&
           modified_ticks(path) == state.local_modified_ticks;
}

std::string content_fingerprint(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error(
            "cannot open file for recovery fingerprint: " + path.string()
        );
    }
    struct DigestContextDeleter {
        void operator()(EVP_MD_CTX* context) const noexcept {
            EVP_MD_CTX_free(context);
        }
    };
    const std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> context{
        EVP_MD_CTX_new()
    };
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("cannot initialize SHA-256 recovery fingerprint");
    }
    std::array<char, 64U * 1024U> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count != 0 &&
            EVP_DigestUpdate(
                context.get(),
                buffer.data(),
                static_cast<std::size_t>(count)
            ) != 1) {
            throw std::runtime_error("cannot update SHA-256 recovery fingerprint");
        }
    }
    if (!input.eof()) {
        throw std::runtime_error(
            "cannot read file for recovery fingerprint: " + path.string()
        );
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1) {
        throw std::runtime_error("cannot finalize SHA-256 recovery fingerprint");
    }
    std::string result;
    result.reserve(static_cast<std::size_t>(digest_size) * 2);
    constexpr std::string_view hex{"0123456789abcdef"};
    for (unsigned int index = 0; index < digest_size; ++index) {
        result.push_back(hex[digest[index] >> 4U]);
        result.push_back(hex[digest[index] & 0x0FU]);
    }
    return result;
}

std::filesystem::path temporary_path_for(
    const std::filesystem::path& destination
) {
    static std::uint64_t sequence = 0;
    return destination.parent_path() /
           std::format(
               ".{}.onedrive-partial-{}-{}",
               destination.filename().string(),
               ::getpid(),
               ++sequence
           );
}

void fsync_directory(const std::filesystem::path& directory) {
    const int descriptor = ::open(
        directory.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC
    );
    if (descriptor == -1) {
        throw std::runtime_error(
            "cannot open download directory '" + directory.string() + "': " +
            std::strerror(errno)
        );
    }
    if (::fsync(descriptor) == -1) {
        const std::string message = std::strerror(errno);
        ::close(descriptor);
        throw std::runtime_error(
            "cannot flush download directory '" + directory.string() + "': " +
            message
        );
    }
    if (::close(descriptor) == -1) {
        throw std::runtime_error(
            "cannot close download directory '" + directory.string() + "': " +
            std::strerror(errno)
        );
    }
}

void ensure_directory_tree(
    const std::filesystem::path& root,
    const std::filesystem::path& directory
) {
    const auto relative = directory.lexically_relative(root);
    if (relative.empty() && directory != root) {
        throw std::runtime_error(
            "local synchronization path escapes the configured directory"
        );
    }

    std::filesystem::path current = root;
    for (const auto& component : relative) {
        current /= component;
        const auto status = std::filesystem::symlink_status(current);
        if (std::filesystem::is_symlink(status)) {
            throw std::runtime_error(
                "local synchronization path contains a symbolic link: " +
                current.string()
            );
        }
        if (std::filesystem::exists(status)) {
            if (!std::filesystem::is_directory(status)) {
                throw std::runtime_error(
                    "local path conflicts with remote directory: " +
                    current.string()
                );
            }
            continue;
        }
        std::filesystem::create_directory(current);
        spdlog::trace("Created local directory '{}'", current.string());
    }
}

}  // namespace onedrive::sync::detail
