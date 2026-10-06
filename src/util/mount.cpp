#include "onedrive/util/mount.hpp"

#include "onedrive/util/path_security.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace onedrive::util {
namespace {

std::string decode_mount_path(std::string_view encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        if (encoded[index] == '\\' && index + 3 < encoded.size() &&
            encoded[index + 1] >= '0' && encoded[index + 1] <= '7' &&
            encoded[index + 2] >= '0' && encoded[index + 2] <= '7' &&
            encoded[index + 3] >= '0' && encoded[index + 3] <= '7') {
            const auto value =
                (encoded[index + 1] - '0') * 64 +
                (encoded[index + 2] - '0') * 8 +
                (encoded[index + 3] - '0');
            decoded.push_back(static_cast<char>(value));
            index += 3;
            continue;
        }
        decoded.push_back(encoded[index]);
    }
    return decoded;
}

}  // namespace

bool is_mount_point(const std::filesystem::path& path) {
    const auto expected = normalized_absolute(path);
    std::ifstream mount_info{"/proc/self/mountinfo"};
    if (!mount_info) {
        throw std::runtime_error(
            "cannot inspect current mounts through /proc/self/mountinfo"
        );
    }

    std::string line;
    while (std::getline(mount_info, line)) {
        std::istringstream fields{line};
        std::string mount_id;
        std::string parent_id;
        std::string device;
        std::string root;
        std::string encoded_mount_point;
        if (!(fields >> mount_id >> parent_id >> device >> root >>
              encoded_mount_point)) {
            continue;
        }
        if (std::filesystem::path{
                decode_mount_path(encoded_mount_point)
            }.lexically_normal() == expected) {
            return true;
        }
    }
    if (mount_info.bad()) {
        throw std::runtime_error(
            "cannot read current mounts from /proc/self/mountinfo"
        );
    }
    return false;
}

void require_sync_mount(
    const std::filesystem::path& sync_data_directory,
    const std::optional<std::filesystem::path>& mount_point
) {
    if (!mount_point) {
        return;
    }
    if (mount_point->empty()) {
        throw std::runtime_error("sync.mount_point must not be empty");
    }

    reject_symlink_components(*mount_point, "sync mount point");
    const auto mount = normalized_absolute(*mount_point);
    const auto sync = normalized_absolute(sync_data_directory);
    if (!path_contains(mount, sync)) {
        throw std::runtime_error(
            "sync.data_directory must be inside sync.mount_point: " +
            mount.string()
        );
    }
    std::error_code error;
    if (!std::filesystem::is_directory(mount, error)) {
        if (error) {
            throw SyncMountUnavailableError(
                "cannot inspect sync.mount_point '" + mount.string() +
                "': " + error.message()
            );
        }
        throw SyncMountUnavailableError(
            "sync.mount_point is not a directory: " + mount.string()
        );
    }
    if (!is_mount_point(mount)) {
        throw SyncMountUnavailableError(
            "configured sync mount point is not currently mounted: " +
            mount.string()
        );
    }
}

}  // namespace onedrive::util
