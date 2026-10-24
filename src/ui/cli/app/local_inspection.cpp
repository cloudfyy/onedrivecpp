#include "info.hpp"
#include "drive_fields.hpp"
#include "inspection.hpp"
#include "onedrive/exit_code.hpp"
#include "sync/download/integrity.hpp"
#include "util/ascii.hpp"
#include <array>
#include <stdexcept>
#include <system_error>

namespace onedrive::app::detail {

namespace {

std::filesystem::space_info
filesystem_space(const std::filesystem::path& requested_path) {
    auto probe = requested_path;
    std::error_code error;
    while (true) {
        const auto status = std::filesystem::symlink_status(probe, error);
        if (!error && std::filesystem::exists(status)) {
            break;
        }
        if (error != std::errc::no_such_file_or_directory) {
            throw std::runtime_error(
                "cannot inspect storage path '" + probe.string() +
                "': " + error.message()
            );
        }
        error.clear();
        const auto parent = probe.parent_path();
        if (parent.empty() || parent == probe) {
            throw std::runtime_error(
                "cannot find an existing filesystem ancestor for '" +
                requested_path.string() + "'"
            );
        }
        probe = parent;
    }
    const auto result = std::filesystem::space(probe, error);
    if (error) {
        throw std::runtime_error(
            "cannot read filesystem space for '" + probe.string() +
            "': " + error.message()
        );
    }
    return result;
}

} // namespace

int show_storage(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
) {
    auto inspection = open_inspection_store(config, runtime_factory);
    std::vector<storage::ItemState> items;
    std::vector<storage::PartialDownload> partials;
    if (inspection.items) {
        items = inspection.items->drive_items(inspection.identity.drive_id);
        partials =
            inspection.items->partial_downloads(inspection.identity.drive_id);
    }

    std::uint64_t tracked_bytes = 0;
    std::size_t tracked_files = 0;
    for (const auto& item : items) {
        if (item.directory) {
            continue;
        }
        ++tracked_files;
        if (item.local_size >= 0) {
            tracked_bytes = checked_add(
                tracked_bytes, static_cast<std::uint64_t>(item.local_size)
            );
        }
    }

    std::uint64_t partial_bytes = 0;
    std::size_t resumable_partials = 0;
    for (const auto& partial : partials) {
        const auto inspected = inspect_partial(inspection.sync_root, partial);
        if (inspected.actual_size) {
            partial_bytes = checked_add(partial_bytes, *inspected.actual_size);
        }
        resumable_partials +=
            inspected.status == PartialStatus::resumable ? 1U : 0U;
    }

    std::error_code root_error;
    const bool root_exists = std::filesystem::is_directory(
        std::filesystem::symlink_status(inspection.sync_root, root_error)
    );
    if (root_error == std::errc::no_such_file_or_directory) {
        root_error.clear();
    } else if (root_error) {
        throw std::runtime_error(
            "cannot inspect synchronization root '" +
            inspection.sync_root.string() + "': " + root_error.message()
        );
    }
    const auto space = filesystem_space(inspection.sync_root);
    console.section(
        "storage",
        "Local synchronization storage:",
        {
            {
                .label = "root:",
                .key = "root",
                .value = inspection.sync_root.string(),
            },
            {
                .label = "root exists:",
                .key = "root_exists",
                .value = root_exists ? "true" : "false",
            },
            {
                .label = "filesystem capacity:",
                .key = "filesystem_capacity",
                .value = format_bytes(space.capacity),
            },
            {
                .label = "filesystem available:",
                .key = "filesystem_available",
                .value = format_bytes(space.available),
            },
            {
                .label = "tracked files:",
                .key = "tracked_files",
                .value = std::to_string(tracked_files),
            },
            {
                .label = "tracked bytes:",
                .key = "tracked_bytes",
                .value = format_bytes(tracked_bytes),
            },
            {
                .label = "partial downloads:",
                .key = "partial_downloads",
                .value = std::to_string(partials.size()),
            },
            {
                .label = "resumable partials:",
                .key = "resumable_partials",
                .value = std::to_string(resumable_partials),
            },
            {
                .label = "partial bytes:",
                .key = "partial_bytes",
                .value = format_bytes(partial_bytes),
            },
            {
                .label = "state database:",
                .key = "state_database",
                .value = inspection.items ? inspection.database_path.string()
                                          : "absent",
            },
        }
    );
    return exit_code::success;
}

int show_partials(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
) {
    auto inspection = open_inspection_store(config, runtime_factory);
    const auto partials =
        inspection.items
            ? inspection.items->partial_downloads(inspection.identity.drive_id)
            : std::vector<storage::PartialDownload>{};
    std::size_t resumable = 0;
    std::uint64_t actual_bytes = 0;
    for (const auto& partial : partials) {
        const auto inspected = inspect_partial(inspection.sync_root, partial);
        resumable += inspected.status == PartialStatus::resumable ? 1U : 0U;
        if (inspected.actual_size) {
            actual_bytes = checked_add(actual_bytes, *inspected.actual_size);
        }
        console.section(
            "partial_download",
            "Partial download:",
            {
                {
                    .label = "remote path:",
                    .key = "remote_path",
                    .value = partial.item.remote_path,
                },
                {
                    .label = "destination:",
                    .key = "destination",
                    .value = partial.item.local_path.string(),
                },
                {
                    .label = "temporary file:",
                    .key = "temporary_path",
                    .value = partial.temporary_path.string(),
                },
                {
                    .label = "status:",
                    .key = "status",
                    .value = std::string{status_name(inspected.status)},
                },
                {
                    .label = "saved download progress:",
                    .key = "completed_bytes",
                    .value = format_bytes(partial.completed_bytes),
                },
                {
                    .label = "total file size:",
                    .key = "expected_bytes",
                    .value =
                        partial.item.size < 0
                            ? "invalid"
                            : format_bytes(
                                  static_cast<std::uint64_t>(partial.item.size)
                              ),
                },
                {
                    .label = "local temporary file size:",
                    .key = "actual_bytes",
                    .value = inspected.actual_size
                                 ? format_bytes(*inspected.actual_size)
                                 : "unavailable",
                },
            }
        );
    }
    if (partials.empty()) {
        console.message(
            cli::MessageKind::information,
            "no_partial_downloads",
            "No partial downloads are recorded."
        );
    }
    console.section(
        "partial_download_summary",
        "Partial download summary:",
        {
            {
                .label = "recorded:",
                .key = "recorded",
                .value = std::to_string(partials.size()),
            },
            {
                .label = "resumable:",
                .key = "resumable",
                .value = std::to_string(resumable),
            },
            {
                .label = "invalid:",
                .key = "invalid",
                .value = std::to_string(partials.size() - resumable),
            },
            {
                .label = "temporary files total:",
                .key = "actual_bytes",
                .value = format_bytes(actual_bytes),
            },
        }
    );
    return exit_code::success;
}

int show_files(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console,
    std::string_view path,
    const std::optional<std::string>& status
) {
    const auto filter = normalized_inspection_path(path);
    auto inspection = open_inspection_store(config, runtime_factory);
    const auto items =
        inspection.items
            ? inspection.items->drive_items(inspection.identity.drive_id)
            : std::vector<storage::ItemState>{};
    std::array<std::size_t, 5> counts{};
    std::size_t inspected_count = 0;
    std::size_t shown = 0;
    for (const auto& item : items) {
        if (item.directory || !path_matches_filter(item.remote_path, filter)) {
            continue;
        }
        const auto inspected = inspect_file(inspection.sync_root, item);
        ++inspected_count;
        ++counts[static_cast<std::size_t>(inspected.status)];
        if (status && *status != status_name(inspected.status)) {
            continue;
        }
        ++shown;
        console.section(
            "downloaded_file",
            "Downloaded file:",
            {
                {
                    .label = "remote path:",
                    .key = "remote_path",
                    .value = item.remote_path,
                },
                {
                    .label = "local path:",
                    .key = "local_path",
                    .value = item.local_path.string(),
                },
                {
                    .label = "status:",
                    .key = "status",
                    .value = std::string{status_name(inspected.status)},
                },
                {
                    .label = "expected size:",
                    .key = "expected_size",
                    .value =
                        item.local_size < 0
                            ? "invalid"
                            : format_bytes(
                                  static_cast<std::uint64_t>(item.local_size)
                              ),
                },
                {
                    .label = "actual size:",
                    .key = "actual_size",
                    .value = inspected.actual_size
                                 ? format_bytes(*inspected.actual_size)
                                 : "unavailable",
                },
            }
        );
    }
    if (shown == 0) {
        console.message(
            cli::MessageKind::information,
            "no_downloaded_files",
            "No downloaded files matched the requested filters."
        );
    }
    console.section(
        "downloaded_file_summary",
        "Downloaded file summary:",
        {
            {
                .label = "inspected:",
                .key = "inspected",
                .value = std::to_string(inspected_count),
            },
            {
                .label = "shown:",
                .key = "shown",
                .value = std::to_string(shown),
            },
            {
                .label = "ok:",
                .key = "ok",
                .value = std::to_string(
                    counts[static_cast<std::size_t>(LocalFileStatus::ok)]
                ),
            },
            {
                .label = "missing:",
                .key = "missing",
                .value = std::to_string(
                    counts[static_cast<std::size_t>(LocalFileStatus::missing)]
                ),
            },
            {
                .label = "modified:",
                .key = "modified",
                .value = std::to_string(
                    counts[static_cast<std::size_t>(LocalFileStatus::modified)]
                ),
            },
            {
                .label = "type changed:",
                .key = "type_changed",
                .value = std::to_string(
                    counts[static_cast<
                        std::size_t>(LocalFileStatus::type_changed)]
                ),
            },
            {
                .label = "outside root:",
                .key = "outside_root",
                .value = std::to_string(
                    counts[static_cast<
                        std::size_t>(LocalFileStatus::outside_root)]
                ),
            },
        }
    );
    return exit_code::success;
}

int verify_files(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console,
    std::string_view path,
    std::string_view mode
) {
    const auto filter = normalized_inspection_path(path);
    auto inspection = open_inspection_store(config, runtime_factory);
    const auto items =
        inspection.items
            ? inspection.items->drive_items(inspection.identity.drive_id)
            : std::vector<storage::ItemState>{};
    std::size_t verified = 0;
    std::size_t failed = 0;
    std::size_t unavailable = 0;
    for (const auto& item : items) {
        if (item.directory || !path_matches_filter(item.remote_path, filter)) {
            continue;
        }
        const auto metadata = inspect_file(inspection.sync_root, item);
        std::string result{status_name(metadata.status)};
        std::string algorithm{"none"};
        if (metadata.status == LocalFileStatus::ok) {
            if (mode == "metadata") {
                result = "verified";
                ++verified;
            } else if (!item.content_hash) {
                result = "hash-unavailable";
                ++unavailable;
            } else {
                algorithm = util::file_hash_algorithm_name(
                    item.content_hash->algorithm
                );
                const auto actual = sync::detail::file_content_hash(
                    item.local_path, item.content_hash->algorithm
                );
                const bool matches =
                    item.content_hash->algorithm ==
                            util::FileHashAlgorithm::quick_xor
                        ? actual == item.content_hash->value
                        : util::ascii_iequals(actual, item.content_hash->value);
                result = matches ? "verified" : "hash-mismatch";
                matches ? ++verified : ++failed;
            }
        } else {
            ++failed;
        }
        console.section(
            "file_verification",
            "File verification:",
            {
                {
                    .label = "remote path:",
                    .key = "remote_path",
                    .value = item.remote_path,
                },
                {
                    .label = "local path:",
                    .key = "local_path",
                    .value = item.local_path.string(),
                },
                {.label = "mode:", .key = "mode", .value = std::string{mode}},
                {.label = "result:", .key = "result", .value = result},
                {
                    .label = "algorithm:",
                    .key = "algorithm",
                    .value = algorithm,
                },
            }
        );
    }
    console.section(
        "file_verification_summary",
        "File verification summary:",
        {
            {
                .label = "verified:",
                .key = "verified",
                .value = std::to_string(verified),
            },
            {
                .label = "failed:",
                .key = "failed",
                .value = std::to_string(failed),
            },
            {
                .label = "hash unavailable:",
                .key = "hash_unavailable",
                .value = std::to_string(unavailable),
            },
        }
    );
    return failed == 0 && unavailable == 0 ? exit_code::success
                                           : exit_code::failure;
}

} // namespace onedrive::app::detail
