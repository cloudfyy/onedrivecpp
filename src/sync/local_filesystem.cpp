#include "local_filesystem.hpp"

#include <openssl/evp.h>
#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
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
namespace {

std::string escaped_path(std::string_view path) {
    std::string escaped;
    escaped.reserve(path.size());
    for (const char byte : path) {
        const auto character = static_cast<unsigned char>(byte);
        if (character == '\\') {
            escaped += "\\\\";
        } else if (character == '\'') {
            escaped += "\\'";
        } else if (character < 0x20U || character == 0x7FU) {
            escaped += std::format("\\x{:02X}", character);
        } else {
            escaped.push_back(static_cast<char>(character));
        }
    }
    return escaped;
}

std::filesystem::path existing_ancestor(std::filesystem::path path) {
    while (!path.empty()) {
        std::error_code error;
        if (std::filesystem::exists(path, error)) {
            return path;
        }
        if (error) {
            throw std::runtime_error(
                "cannot inspect synchronization path '" + path.string() +
                "' while validating remote filenames: " + error.message()
            );
        }
        const auto parent = path.parent_path();
        if (parent == path) {
            break;
        }
        path = parent;
    }
    return ".";
}

std::size_t filesystem_limit(
    const std::filesystem::path& sync_directory,
    int name,
    std::size_t fallback,
    std::string_view description
) {
    const auto probe = existing_ancestor(sync_directory);
    errno = 0;
    const long limit = ::pathconf(probe.c_str(), name);
    if (limit > 0) {
        return static_cast<std::size_t>(limit);
    }
    if (limit == -1 && errno == 0) {
        return fallback;
    }
    throw std::runtime_error(
        "cannot determine the target filesystem " + std::string{description} +
        " at '" + probe.string() + "': " + std::strerror(errno)
    );
}

[[noreturn]] void invalid_remote_path(
    std::string_view remote_path,
    std::string_view reason
) {
    throw InvalidRemotePathError(
        "invalid Microsoft Graph remote path '" + escaped_path(remote_path) +
        "': " + std::string{reason}
    );
}

}  // namespace

std::filesystem::path local_path_for(
    const std::filesystem::path& sync_directory,
    const std::string& remote_path
) {
    if (remote_path.empty()) {
        invalid_remote_path(remote_path, "the path is empty");
    }
    if (remote_path.front() == '/') {
        invalid_remote_path(remote_path, "absolute paths are not allowed");
    }

    const auto name_limit = filesystem_limit(
        sync_directory,
        _PC_NAME_MAX,
        NAME_MAX,
        "filename length limit"
    );
    std::size_t component_begin = 0;
    while (component_begin <= remote_path.size()) {
        const auto separator = remote_path.find('/', component_begin);
        const auto component_end =
            separator == std::string::npos ? remote_path.size() : separator;
        const std::string_view component{
            remote_path.data() + component_begin,
            component_end - component_begin
        };
        if (component.empty()) {
            invalid_remote_path(
                remote_path,
                "empty path components are not allowed"
            );
        }
        if (component == "." || component == "..") {
            invalid_remote_path(
                remote_path,
                "reserved component '" + std::string{component} +
                    "' is not allowed"
            );
        }
        if (component.contains('\0')) {
            invalid_remote_path(
                remote_path,
                "component '" + escaped_path(component) +
                    "' contains a NUL byte"
            );
        }
        for (const char byte : component) {
            const auto character = static_cast<unsigned char>(byte);
            if (character < 0x20U || character == 0x7FU) {
                invalid_remote_path(
                    remote_path,
                    "component '" + escaped_path(component) +
                        "' contains unsupported control byte " +
                        std::format("0x{:02X}", character)
                );
            }
        }
        if (component.size() > name_limit) {
            invalid_remote_path(
                remote_path,
                "component '" + escaped_path(component) + "' is " +
                    std::to_string(component.size()) +
                    " bytes, exceeding the target filesystem limit of " +
                    std::to_string(name_limit) + " bytes"
            );
        }
        if (separator == std::string::npos) {
            break;
        }
        component_begin = separator + 1;
    }

    const std::filesystem::path local_path =
        sync_directory / std::filesystem::path{remote_path};
    const auto path_limit = filesystem_limit(
        sync_directory,
        _PC_PATH_MAX,
        PATH_MAX,
        "path length limit"
    );
    if (local_path.native().size() >= path_limit) {
        invalid_remote_path(
            remote_path,
            "the resulting local path is " +
                std::to_string(local_path.native().size()) +
                " bytes, exceeding the target filesystem limit of " +
                std::to_string(path_limit - 1) + " bytes"
        );
    }
    return local_path;
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

LocalFileBaseline capture_local_file_baseline(
    const std::filesystem::path& path
) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            return {};
        }
        throw std::runtime_error(
            "cannot inspect local download destination '" + path.string() +
            "': " + error.message()
        );
    }
    if (!std::filesystem::exists(status)) {
        return {};
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw LocalPathConflictError(
            "local download destination is not a regular file: " +
            path.string()
        );
    }

    const auto size_before = std::filesystem::file_size(path);
    if (size_before > static_cast<std::uintmax_t>(
                          std::numeric_limits<std::int64_t>::max()
                      )) {
        throw std::runtime_error(
            "local download destination is too large to track: " +
            path.string()
        );
    }
    const auto ticks_before = modified_ticks(path);
    auto fingerprint = content_fingerprint(path);
    const auto size_after = std::filesystem::file_size(path);
    const auto ticks_after = modified_ticks(path);
    if (size_before != size_after || ticks_before != ticks_after) {
        throw LocalModificationConflictError(
            "local file changed while capturing the download baseline: " +
            path.string()
        );
    }
    return {
        .existed = true,
        .size = static_cast<std::int64_t>(size_after),
        .modified_ticks = ticks_after,
        .fingerprint = std::move(fingerprint),
    };
}

bool local_file_matches_baseline(
    const std::filesystem::path& path,
    const LocalFileBaseline& baseline
) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            return !baseline.existed;
        }
        throw std::runtime_error(
            "cannot inspect local download destination '" + path.string() +
            "': " + error.message()
        );
    }
    if (!baseline.existed) {
        return !std::filesystem::exists(status);
    }
    if (!std::filesystem::is_regular_file(status)) {
        return false;
    }
    const auto size_before = std::filesystem::file_size(path);
    if (size_before > static_cast<std::uintmax_t>(
                          std::numeric_limits<std::int64_t>::max()
                      ) ||
        static_cast<std::int64_t>(size_before) != baseline.size ||
        modified_ticks(path) != baseline.modified_ticks) {
        return false;
    }
    const auto fingerprint = content_fingerprint(path);
    const auto size_after = std::filesystem::file_size(path);
    return size_before == size_after &&
           static_cast<std::int64_t>(size_after) == baseline.size &&
           modified_ticks(path) == baseline.modified_ticks &&
           fingerprint == baseline.fingerprint;
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
    std::array<char, std::size_t{64} * 1024U> buffer{};
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
    static std::atomic_uint64_t sequence{0};
    return destination.parent_path() /
           std::format(
               ".{}.onedrive-partial-{}-{}",
               destination.filename().string(),
               ::getpid(),
               sequence.fetch_add(1, std::memory_order_relaxed) + 1
           );
}

bool paths_share_parent(
    const std::filesystem::path& left,
    const std::filesystem::path& right
) {
    return left.lexically_normal().parent_path() ==
           right.lexically_normal().parent_path();
}

bool is_temporary_path_for(
    const std::filesystem::path& destination,
    const std::filesystem::path& candidate
) {
    const auto prefix =
        "." + destination.filename().string() + ".onedrive-partial-";
    return paths_share_parent(destination, candidate) &&
           candidate.lexically_normal().filename().string().starts_with(
               prefix
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
            throw LocalPathConflictError(
                "local synchronization path contains a symbolic link: " +
                current.string()
            );
        }
        if (std::filesystem::exists(status)) {
            if (!std::filesystem::is_directory(status)) {
                throw LocalPathConflictError(
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
