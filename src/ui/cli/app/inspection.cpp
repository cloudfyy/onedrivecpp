#include "inspection.hpp"
#include "onedrive/account/account_state.hpp"
#include "sync/download/integrity.hpp"
#include "sync/filesystem/operations.hpp"
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace onedrive::app::detail {

InspectionStore open_inspection_store(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    account::DriveIdentity identity
) {
    const auto account_paths =
        account::AccountState::locate(config.state_directory, identity);
    const auto database_path = account_paths.drive_directory / "items.sqlite3";
    std::error_code error;
    const auto database_status =
        std::filesystem::symlink_status(database_path, error);
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
    } else if (error) {
        throw std::runtime_error(
            "cannot inspect synchronization state database '" +
            database_path.string() + "': " + error.message()
        );
    }

    std::unique_ptr<storage::ItemStore> items;
    if (std::filesystem::exists(database_status)) {
        if (!std::filesystem::is_regular_file(database_status)) {
            throw std::runtime_error(
                "synchronization state database is not a regular file: " +
                database_path.string()
            );
        }
        items = runtime_factory.create_item_store(config, identity);
        items->open_read_only();
    }
    auto sync_root = account::AccountState::drive_data_directory(
        config.sync_data_directory, identity
    );
    return {
        .identity = std::move(identity),
        .sync_root = std::move(sync_root),
        .database_path = database_path,
        .items = std::move(items),
    };
}

InspectionStore open_inspection_store(
    const config::Config& config, const RuntimeFactory& runtime_factory
) {
    return open_inspection_store(
        config,
        runtime_factory,
        runtime_factory.create_graph_info_client(config)->drive_identity()
    );
}

bool path_is_within(
    const std::filesystem::path& root, const std::filesystem::path& path
) {
    const auto relative =
        path.lexically_normal().lexically_relative(root.lexically_normal());
    return !relative.empty() && !relative.is_absolute() &&
           *relative.begin() != "..";
}

std::uint64_t checked_add(std::uint64_t total, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        throw std::runtime_error("local synchronization byte count overflow");
    }
    return total + value;
}

std::string_view status_name(LocalFileStatus status) {
    switch (status) {
    case LocalFileStatus::ok:
        return "ok";
    case LocalFileStatus::missing:
        return "missing";
    case LocalFileStatus::modified:
        return "modified";
    case LocalFileStatus::type_changed:
        return "type-changed";
    case LocalFileStatus::outside_root:
        return "outside-root";
    }
    return "unknown";
}

InspectedFile inspect_file(
    const std::filesystem::path& root, const storage::ItemState& item
) {
    if (!path_is_within(root, item.local_path)) {
        return {
            .item = &item,
            .status = LocalFileStatus::outside_root,
            .actual_size = std::nullopt,
        };
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(item.local_path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return {
            .item = &item,
            .status = LocalFileStatus::missing,
            .actual_size = std::nullopt,
        };
    }
    if (error) {
        throw std::runtime_error(
            "cannot inspect downloaded file '" + item.local_path.string() +
            "': " + error.message()
        );
    }
    if (!std::filesystem::exists(status)) {
        return {
            .item = &item,
            .status = LocalFileStatus::missing,
            .actual_size = std::nullopt,
        };
    }
    if (!std::filesystem::is_regular_file(status)) {
        return {
            .item = &item,
            .status = LocalFileStatus::type_changed,
            .actual_size = std::nullopt,
        };
    }
    const auto actual_size = std::filesystem::file_size(item.local_path, error);
    if (error) {
        throw std::runtime_error(
            "cannot read downloaded file size '" + item.local_path.string() +
            "': " + error.message()
        );
    }
    return {
        .item = &item,
        .status = sync::detail::local_snapshot_matches(item, item.local_path)
                      ? LocalFileStatus::ok
                      : LocalFileStatus::modified,
        .actual_size = static_cast<std::uint64_t>(actual_size),
    };
}

std::string normalized_inspection_path(std::string_view path) {
    std::string result{path};
    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    if (result.empty()) {
        return {};
    }
    if (result.front() == '/') {
        throw std::invalid_argument(
            "inspect files PATH must be Drive-relative"
        );
    }
    std::size_t begin = 0;
    while (begin <= result.size()) {
        const auto end = result.find('/', begin);
        const auto component = std::string_view{result}.substr(
            begin,
            end == std::string::npos ? result.size() - begin : end - begin
        );
        if (component.empty() || component == "." || component == "..") {
            throw std::invalid_argument(
                "inspect files PATH contains an invalid component"
            );
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

bool path_matches_filter(std::string_view path, std::string_view filter) {
    return filter.empty() || path == filter ||
           (path.size() > filter.size() && path.starts_with(filter) &&
            path[filter.size()] == '/');
}

std::string_view status_name(PartialStatus status) {
    switch (status) {
    case PartialStatus::resumable:
        return "resumable";
    case PartialStatus::missing:
        return "missing";
    case PartialStatus::type_changed:
        return "type-changed";
    case PartialStatus::outside_root:
        return "outside-root";
    case PartialStatus::path_mismatch:
        return "path-mismatch";
    case PartialStatus::invalid_checkpoint:
        return "invalid-checkpoint";
    }
    return "unknown";
}

InspectedPartial inspect_partial(
    const std::filesystem::path& root, const storage::PartialDownload& partial
) {
    if (!path_is_within(root, partial.item.local_path)) {
        return {
            .partial = &partial,
            .status = PartialStatus::outside_root,
            .actual_size = std::nullopt,
        };
    }
    if (!sync::detail::is_temporary_path_for(
            partial.item.local_path, partial.temporary_path
        )) {
        return {
            .partial = &partial,
            .status = PartialStatus::path_mismatch,
            .actual_size = std::nullopt,
        };
    }
    if (partial.item.size < 0 ||
        partial.completed_bytes >
            static_cast<std::uint64_t>(partial.item.size)) {
        return {
            .partial = &partial,
            .status = PartialStatus::invalid_checkpoint,
            .actual_size = std::nullopt,
        };
    }
    std::error_code error;
    const auto status =
        std::filesystem::symlink_status(partial.temporary_path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return {
            .partial = &partial,
            .status = PartialStatus::missing,
            .actual_size = std::nullopt,
        };
    }
    if (error) {
        throw std::runtime_error(
            "cannot inspect partial download '" +
            partial.temporary_path.string() + "': " + error.message()
        );
    }
    if (!std::filesystem::exists(status)) {
        return {
            .partial = &partial,
            .status = PartialStatus::missing,
            .actual_size = std::nullopt,
        };
    }
    if (!std::filesystem::is_regular_file(status)) {
        return {
            .partial = &partial,
            .status = PartialStatus::type_changed,
            .actual_size = std::nullopt,
        };
    }
    const auto actual_size =
        std::filesystem::file_size(partial.temporary_path, error);
    if (error) {
        throw std::runtime_error(
            "cannot read partial download size '" +
            partial.temporary_path.string() + "': " + error.message()
        );
    }
    return {
        .partial = &partial,
        .status = actual_size < partial.completed_bytes
                      ? PartialStatus::invalid_checkpoint
                      : PartialStatus::resumable,
        .actual_size = static_cast<std::uint64_t>(actual_size),
    };
}

} // namespace onedrive::app::detail
