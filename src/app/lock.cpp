#include "lock.hpp"

#include "onedrive/util/path_security.hpp"
#include "onedrive/util/system_error.hpp"
#include "onedrive/util/sha256.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "onedrive/config/config.hpp"
#include "util/private_file.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace onedrive::app::detail {
namespace {

constexpr mode_t private_file_mode = S_IRUSR | S_IWUSR;
constexpr auto maximum_metadata_size = util::maximum_private_file_size;

using Json = nlohmann::json;

LockOwner current_owner(
    const std::filesystem::path& state_directory,
    const std::filesystem::path& sync_directory,
    std::string_view operation
) {
    return {
        .pid = ::getpid(),
        .operation = std::string{operation},
        .started_at = std::format(
            "{:%FT%TZ}",
            std::chrono::floor<std::chrono::seconds>(
                std::chrono::system_clock::now()
            )
        ),
        .state_directory = util::normalized_absolute(state_directory),
        .sync_directory = sync_directory,
    };
}

Json owner_json(const LockOwner& owner) {
    return {
        {"pid", owner.pid},
        {"operation", owner.operation},
        {"started_at", owner.started_at},
        {"state_directory", owner.state_directory.string()},
        {"sync_directory", owner.sync_directory.string()},
    };
}

std::string read_metadata(int descriptor, const std::filesystem::path& path) {
    return util::read_private_file(
        descriptor,
        path,
        "runtime lock",
        {
            .required_owner = ::geteuid(),
            .exact_permissions = std::nullopt,
            .permission_requirement = "must be owned by the current user",
            .require_single_link = true,
        }
    );
}

LockOwner parse_owner(const Json& json) {
    LockOwner result{
        .pid = json.at("pid").get<std::int64_t>(),
        .operation = json.at("operation").get<std::string>(),
        .started_at = json.at("started_at").get<std::string>(),
        .state_directory = json.at("state_directory").get<std::string>(),
        .sync_directory = json.at("sync_directory").get<std::string>(),
    };
    if (result.pid <= 0 || result.operation.empty() ||
        result.started_at.empty() || !result.state_directory.is_absolute()) {
        throw std::runtime_error("invalid lock owner metadata");
    }
    return result;
}

void write_metadata(int descriptor, const Json& metadata) {
    const auto content = metadata.dump() + '\n';
    if (content.size() > maximum_metadata_size) {
        throw std::runtime_error("lock metadata exceeds the size limit");
    }
    if (::ftruncate(descriptor, 0) == -1) {
        throw std::runtime_error(
            "cannot truncate lock metadata: " +
            util::system_error_message(errno)
        );
    }
    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto bytes = ::pwrite(
            descriptor,
            content.data() + offset,
            content.size() - offset,
            static_cast<off_t>(offset)
        );
        if (bytes == -1 && errno == EINTR) {
            continue;
        }
        if (bytes <= 0) {
            throw std::runtime_error(
                "cannot write lock metadata: " +
                (bytes == 0 ? std::string{"short write"}
                            : util::system_error_message(errno))
            );
        }
        offset += static_cast<std::size_t>(bytes);
    }
    if (::fsync(descriptor) == -1) {
        throw std::runtime_error(
            "cannot flush lock metadata: " + util::system_error_message(errno)
        );
    }
}

util::UniqueFD open_lock(const std::filesystem::path& path) {
    auto descriptor =
        util::open_path_no_symlinks(path, O_RDWR | O_CREAT, private_file_mode);
    static_cast<void>(util::secure_owned_regular_file(
        descriptor.get(), path, private_file_mode, "runtime lock"
    ));
    return descriptor;
}

bool try_lock(int descriptor) {
    while (::flock(descriptor, LOCK_EX | LOCK_NB) == -1) {
        if (errno == EINTR) {
            continue;
        }
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return false;
        }
        throw std::runtime_error(
            "cannot acquire runtime lock: " + util::system_error_message(errno)
        );
    }
    return true;
}

using DirectoryIdentity = util::FilesystemIdentity;

Json identity_json(const DirectoryIdentity& identity) {
    return {{"device", identity.device}, {"inode", identity.inode}};
}

DirectoryIdentity identity_from_json(const Json& json) {
    return {
        .device = json.at("device").get<std::uint64_t>(),
        .inode = json.at("inode").get<std::uint64_t>(),
    };
}

struct RootIdentity {
    std::optional<DirectoryIdentity> root;
    std::vector<DirectoryIdentity> ancestors;
};

RootIdentity inspect_root(const std::filesystem::path& root) {
    RootIdentity result;
    for (auto path = root;; path = path.parent_path()) {
        struct stat status{};
        if (::lstat(path.c_str(), &status) == -1) {
            if (errno != ENOENT) {
                throw std::runtime_error(
                    "cannot inspect synchronization lock directory '" +
                    path.string() + "': " + util::system_error_message(errno)
                );
            }
        } else {
            if (!S_ISDIR(status.st_mode)) {
                throw std::runtime_error(
                    "synchronization lock path is not a directory: " +
                    path.string()
                );
            }
            const auto identity = util::filesystem_identity(status);
            result.ancestors.push_back(identity);
            if (path == root) {
                result.root = identity;
            }
        }
        if (path == path.root_path()) {
            return result;
        }
    }
}

bool identities_overlap(const RootIdentity& left, const RootIdentity& right) {
    return (left.root && std::ranges::find(right.ancestors, *left.root) !=
                             right.ancestors.end()) ||
           (right.root && std::ranges::find(left.ancestors, *right.root) !=
                              left.ancestors.end());
}

util::UniqueFD acquire_sync_lock(
    const std::filesystem::path& requested,
    const std::filesystem::path& state_directory,
    std::string_view operation
) {
    const auto root =
        config::validate_sync_state_directories(requested, state_directory);
    util::reject_symlink_components(requested, "sync directory");
    const auto registry = std::filesystem::path{"/tmp"} /
                          ("onedrive-cpp-locks-" + std::to_string(::geteuid()));
    if (::mkdir(registry.c_str(), S_IRWXU) == -1 && errno != EEXIST) {
        throw std::runtime_error(
            "cannot create synchronization lock registry: " +
            util::system_error_message(errno)
        );
    }
    util::secure_owned_directory(
        registry, S_IRWXU, "synchronization lock registry"
    );
    auto gate = open_lock(registry / "registry.lock");
    while (::flock(gate.get(), LOCK_EX) == -1) {
        if (errno != EINTR) {
            throw std::runtime_error(
                "cannot lock synchronization registry: " +
                util::system_error_message(errno)
            );
        }
    }
    const auto identity = inspect_root(root);
    for (const auto& entry : std::filesystem::directory_iterator{registry}) {
        if (entry.path().extension() != ".lease") {
            continue;
        }
        const auto lease = util::open_path_no_symlinks(entry.path(), O_RDWR);
        static_cast<void>(util::inspect_owned_regular_file(
            lease.get(), entry.path(), "synchronization lease"
        ));
        if (try_lock(lease.get())) {
            if (!std::filesystem::remove(entry.path())) {
                throw std::runtime_error(
                    "cannot remove stale synchronization lease"
                );
            }
            continue;
        }
        const auto metadata =
            Json::parse(read_metadata(lease.get(), entry.path()));
        if (metadata.at("version").get<int>() != 1) {
            throw std::runtime_error(
                "unsupported synchronization lease version"
            );
        }
        const auto owner = parse_owner(metadata);
        if (!owner.sync_directory.is_absolute()) {
            throw std::runtime_error("invalid synchronization lease directory");
        }
        RootIdentity held{
            .root = std::nullopt,
            .ancestors = {},
        };
        const auto& ancestors = metadata.at("ancestors");
        if (!ancestors.is_array()) {
            throw std::runtime_error("invalid synchronization lease ancestors");
        }
        for (const auto& ancestor : ancestors) {
            held.ancestors.push_back(identity_from_json(ancestor));
        }
        if (!metadata.at("root_identity").is_null()) {
            held.root = identity_from_json(metadata.at("root_identity"));
        }
        if (util::path_contains(root, owner.sync_directory) ||
            util::path_contains(owner.sync_directory, root) ||
            identities_overlap(identity, held) ||
            identities_overlap(identity, inspect_root(owner.sync_directory))) {
            throw RuntimeBusyError{"sync_directory", root, owner};
        }
    }
    auto lease =
        open_lock(registry / (util::sha256_hex(root.string()) + ".lease"));
    if (!try_lock(lease.get())) {
        throw std::runtime_error(
            "synchronization registry changed while locked"
        );
    }
    auto metadata = owner_json(current_owner(state_directory, root, operation));
    metadata["version"] = 1;
    metadata["ancestors"] = Json::array();
    for (const auto& ancestor : identity.ancestors) {
        metadata["ancestors"].push_back(identity_json(ancestor));
    }
    metadata["root_identity"] =
        identity.root ? identity_json(*identity.root) : Json(nullptr);
    write_metadata(lease.get(), metadata);
    return lease;
}

} // namespace

RuntimeBusyError::RuntimeBusyError(
    std::string resource_value,
    std::filesystem::path requested,
    LockOwner owner_value
)
    : std::runtime_error{
          "another onedrive-cpp process is already using this " +
          std::string{resource_value == "state_directory" ? "state directory" :
                                                         "synchronization directory or an overlapping directory"} +
          ": '" + requested.string() + "'" +
          (owner_value.pid > 0
               ? "; owner PID " + std::to_string(owner_value.pid) +
                     ", operation " + owner_value.operation +
                     ", started " + owner_value.started_at +
                     ", state '" + owner_value.state_directory.string() +
                     "', sync '" + owner_value.sync_directory.string() + "'"
               : "; owner metadata is not available")},
      resource{std::move(resource_value)},
      requested_directory{std::move(requested)},
      owner{std::move(owner_value)} {
}

void report_runtime_busy(
    const RuntimeBusyError& error, const events::Observer& observer
) {
    observer.section(
        "runtime_busy",
        "Synchronization resource is busy:",
        {
            {.label = "resource:", .key = "resource", .value = error.resource},
            {.label = "requested directory:",
             .key = "requested_directory",
             .value = error.requested_directory.string()},
            {.label = "PID:",
             .key = "pid",
             .value = error.owner.pid > 0 ? std::to_string(error.owner.pid)
                                          : "unknown"},
            {.label = "operation:",
             .key = "operation",
             .value = error.owner.operation},
            {.label = "started:",
             .key = "started_at",
             .value = error.owner.started_at},
            {.label = "state directory:",
             .key = "state_directory",
             .value = error.owner.state_directory.string()},
            {.label = "sync directory:",
             .key = "sync_directory",
             .value = error.owner.sync_directory.string()},
        }
    );
    observer.message(events::MessageKind::error, "runtime_busy", error.what());
    if (error.owner.operation == "monitor") {
        observer.message(
            events::MessageKind::information,
            "monitor_control_hint",
            "Use 'onedrive-cpp monitor status|trigger|stop --config "
            "<owner-config>' "
            "to explicitly control the Monitor using the owner's state "
            "directory. "
            "Ordinary synchronization is not forwarded or queued."
        );
    }
}

onedrive::util::UniqueFD acquire_runtime_lock(
    const std::filesystem::path& state_directory, std::string_view operation
) {
    const auto path = state_directory / "onedrive-cpp.lock";
    auto descriptor = open_lock(path);
    if (!try_lock(descriptor.get())) {
        const auto content = read_metadata(descriptor.get(), path);
        LockOwner owner;
        const auto metadata = Json::parse(content, nullptr, false);
        if (metadata.is_object()) {
            owner = parse_owner(metadata);
        }
        throw RuntimeBusyError{
            "state_directory", state_directory, std::move(owner)
        };
    }
    write_metadata(
        descriptor.get(),
        owner_json(current_owner(state_directory, {}, operation))
    );
    return descriptor;
}

util::UniqueFD acquire_sync_directory_lock(
    const std::filesystem::path& sync_directory,
    const std::filesystem::path& state_directory,
    std::string_view operation,
    const events::Observer& observer
) {
    try {
        return acquire_sync_lock(sync_directory, state_directory, operation);
    } catch (const RuntimeBusyError& error) {
        report_runtime_busy(error, observer);
        throw;
    }
}

}  // namespace onedrive::app::detail
