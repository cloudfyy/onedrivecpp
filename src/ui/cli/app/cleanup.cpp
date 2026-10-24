#include "info.hpp"
#include "inspection.hpp"
#include "app/lock.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/util/path_security.hpp"
#include "sync/filesystem/operations.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include <algorithm>
#include <fcntl.h>
#include <format>
#include <map>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>

namespace onedrive::app::detail {

namespace {

bool partial_filename(std::string_view name) {
    constexpr std::string_view marker = ".onedrive-partial-";
    const auto position = name.rfind(marker);
    if (!name.starts_with('.') || position == std::string_view::npos ||
        position <= 1) {
        return false;
    }
    const auto suffix = name.substr(position + marker.size());
    const auto separator = suffix.find('-');
    if (separator == std::string_view::npos) {
        return false;
    }
    const auto positive_integer = [](std::string_view value) {
        return !value.empty() && value.front() >= '1' && value.front() <= '9' &&
               std::ranges::all_of(value, [](char digit) {
                   return digit >= '0' && digit <= '9';
               });
    };
    return positive_integer(suffix.substr(0, separator)) &&
           positive_integer(suffix.substr(separator + 1));
}

struct CleanupCandidate {
    std::filesystem::path path;
    std::optional<std::string> remote_id;
    std::optional<struct stat> file;
};

std::optional<struct stat> cleanup_file(
    const std::filesystem::path& path, const sync::detail::SafeSyncRoot* root
) {
    util::reject_symlink_components(path, "partial cleanup");
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && !std::filesystem::exists(status))) {
        return std::nullopt;
    }
    if (error) {
        throw std::runtime_error(
            "cannot inspect cleanup file '" + path.string() +
            "': " + error.message()
        );
    }
    if (!root || !std::filesystem::is_regular_file(status)) {
        throw std::runtime_error(
            "cleanup candidate is not a regular file: " + path.string()
        );
    }
    auto file = root->open(path, O_RDONLY | O_NONBLOCK);
    return util::inspect_owned_regular_file(file.get(), path, "cleanup file");
}

bool same_cleanup_file(const struct stat& before, const struct stat& after) {
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
           before.st_size == after.st_size && before.st_uid == after.st_uid &&
           before.st_mode == after.st_mode &&
           before.st_nlink == after.st_nlink &&
           before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
           before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
           before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
           before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}

std::set<std::filesystem::path> cleanup_protected_paths(
    storage::ItemStore* items, const std::string& drive_id
) {
    std::set<std::filesystem::path> paths;
    const auto add = [&](const std::filesystem::path& path) {
        if (!path.empty()) {
            paths.insert(path.lexically_normal());
        }
    };
    if (!items) {
        return paths;
    }
    for (const auto& item : items->drive_items(drive_id)) {
        add(item.local_path);
    }
    for (const auto& pending : items->pending_downloads(drive_id)) {
        add(pending.item.local_path);
        add(pending.temporary_path);
        add(pending.backup_path);
    }
    for (const auto& pending : items->pending_uploads(drive_id)) {
        add(pending.local_path);
        add(pending.snapshot_path);
    }
    for (const auto& pending : items->pending_deletes(drive_id)) {
        add(pending.local_path);
    }
    for (const auto& pending : items->pending_moves(drive_id)) {
        add(pending.source_path);
        add(pending.destination_path);
        add(pending.staging_path);
    }
    for (const auto& pending : items->pending_remote_moves(drive_id)) {
        add(pending.source_local_path);
        add(pending.destination_local_path);
    }
    return paths;
}

} // namespace

int cleanup_state(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console,
    bool dry_run,
    bool assume_yes
) {
    dry_run = dry_run || config.dry_run;
    auto inspection = open_inspection_store(config, runtime_factory);
    const auto sync_lock = acquire_sync_directory_lock(
        inspection.sync_root, config.state_directory, "cleanup_state", console
    );
    const auto partials =
        inspection.items
            ? inspection.items->partial_downloads(inspection.identity.drive_id)
            : std::vector<storage::PartialDownload>{};
    auto protected_paths = cleanup_protected_paths(
        inspection.items.get(), inspection.identity.drive_id
    );
    std::map<std::filesystem::path, std::size_t> recorded;
    for (const auto& partial : partials) {
        ++recorded[partial.temporary_path.lexically_normal()];
        protected_paths.insert(partial.item.local_path.lexically_normal());
    }
    util::reject_symlink_components(inspection.sync_root, "cleanup root");
    std::optional<sync::detail::SafeSyncRoot> safe_root;
    std::error_code root_error;
    if (std::filesystem::is_directory(inspection.sync_root, root_error)) {
        safe_root.emplace(inspection.sync_root);
    } else if (root_error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error(
            "cannot inspect synchronization root for cleanup: " +
            (root_error ? root_error.message() : "not a directory")
        );
    }
    std::vector<CleanupCandidate> candidates;
    std::size_t retained = 0;
    std::size_t skipped = 0;
    std::size_t invalid_records = 0;
    std::size_t record_only = 0;
    std::size_t orphaned = 0;
    std::size_t planned_files = 0;
    std::uint64_t planned_bytes = 0;
    const auto skip = [&](const std::filesystem::path& path,
                          const std::string& reason) {
        ++skipped;
        console.message(
            cli::MessageKind::warning,
            "cleanup_skipped",
            "Kept '" + path.string() + "': " + reason
        );
    };
    const auto plan = [&](CleanupCandidate candidate,
                          std::string reason,
                          const storage::PartialDownload* partial = nullptr,
                          std::string_view status = {}) {
        if (candidate.file) {
            ++planned_files;
            planned_bytes = checked_add(
                planned_bytes,
                static_cast<std::uint64_t>(candidate.file->st_size)
            );
        } else {
            ++record_only;
        }
        candidate.remote_id ? ++invalid_records : ++orphaned;
        std::vector<cli::Field> fields{
            {.label = "path:", .key = "path", .value = candidate.path.string()},
            {.label = "reason:", .key = "reason", .value = std::move(reason)},
            {.label = "action:",
             .key = "action",
             .value = candidate.file
                          ? (partial ? "remove temporary file and record"
                                     : "remove temporary file")
                          : "remove missing-file record"},
            {.label = "file size (bytes):",
             .key = "bytes",
             .value = candidate.file ? std::to_string(candidate.file->st_size)
                                     : "0"},
        };
        if (partial) {
            fields.push_back(
                {.label = "remote path:",
                 .key = "remote_path",
                 .value = partial->item.remote_path}
            );
            fields.push_back(
                {.label = "temporary path:",
                 .key = "temporary_path",
                 .value = candidate.path.string()}
            );
            fields.push_back(
                {.label = "status:",
                 .key = "status",
                 .value = std::string{status}}
            );
        }
        console.section(
            candidate.remote_id ? "cleanup_partial" : "cleanup_orphan",
            "Partial cleanup candidate:",
            fields
        );
        candidates.push_back(std::move(candidate));
    };
    for (const auto& partial : partials) {
        const auto path = partial.temporary_path.lexically_normal();
        if (!path_is_within(inspection.sync_root, partial.item.local_path) ||
            !path_is_within(inspection.sync_root, path) ||
            !partial_filename(path.filename().string()) ||
            !sync::detail::is_temporary_path_for(
                partial.item.local_path, path
            )) {
            skip(
                path,
                "path is outside this Drive or is not a recognized temporary "
                "file"
            );
            continue;
        }
        if (protected_paths.contains(path) || recorded.at(path) != 1) {
            skip(
                path,
                "file is referenced by another saved item or unfinished "
                "operation"
            );
            continue;
        }
        try {
            const auto file =
                cleanup_file(path, safe_root ? &*safe_root : nullptr);
            const bool valid_progress =
                partial.item.size >= 0 &&
                partial.completed_bytes <=
                    static_cast<std::uint64_t>(partial.item.size);
            if (file && valid_progress &&
                static_cast<std::uint64_t>(file->st_size) >=
                    partial.completed_bytes) {
                ++retained;
                continue;
            }
            plan(
                {path, partial.item.remote_id, file},
                !file ? "temporary file is missing"
                : !valid_progress
                    ? "saved download size or progress is invalid"
                    : "temporary file is smaller than saved progress",
                &partial,
                file ? "invalid-checkpoint" : "missing"
            );
        } catch (const std::runtime_error& error) {
            skip(path, error.what());
        }
    }
    if (safe_root) {
        // Finish the scan before deleting anything; an unreadable subtree
        // aborts the plan.
        for (const auto& entry : std::filesystem::recursive_directory_iterator{
                 inspection.sync_root
             }) {
            const auto path = entry.path().lexically_normal();
            if (!partial_filename(path.filename().string()) ||
                recorded.contains(path)) {
                continue;
            }
            if (protected_paths.contains(path)) {
                ++retained;
                continue;
            }
            try {
                const auto file = cleanup_file(path, &*safe_root);
                if (file) {
                    plan(
                        {path, std::nullopt, file},
                        "no saved download or unfinished operation references "
                        "this file"
                    );
                }
            } catch (const std::runtime_error& error) {
                skip(path, error.what());
            }
        }
    }
    console.section(
        "cleanup_summary",
        "Partial cleanup summary:",
        {
            {
                .label = "invalid records:",
                .key = "invalid_records",
                .value = std::to_string(invalid_records),
            },
            {
                .label = "orphaned files:",
                .key = "orphaned_files",
                .value = std::to_string(orphaned),
            },
            {.label = "retained:",
             .key = "retained",
             .value = std::to_string(retained)},
            {.label = "record only:",
             .key = "record_only",
             .value = std::to_string(record_only)},
            {.label = "files to remove:",
             .key = "planned_files",
             .value = std::to_string(planned_files)},
            {.label = "file bytes to remove:",
             .key = "planned_bytes",
             .value = std::to_string(planned_bytes)},
            {.label = "skipped:",
             .key = "skipped",
             .value = std::to_string(skipped)},
            {
                .label = "mode:",
                .key = "mode",
                .value = dry_run ? "dry-run" : "cleanup",
            },
        }
    );
    if (dry_run || candidates.empty()) {
        return skipped == 0 ? exit_code::success : exit_code::failure;
    }
    if (!assume_yes) {
        if (console.output_mode() == cli::OutputMode::json) {
            console.message(
                cli::MessageKind::error,
                "confirmation_required",
                "Cleanup requires --yes with JSON output."
            );
            return exit_code::failure;
        }
        if (!console.confirm(
                "cleanup_confirmation",
                "Type the configured drive reference '" +
                    inspection.identity.configured_drive_id +
                    "' to confirm cleanup: ",
                inspection.identity.configured_drive_id
            )) {
            console.message(
                cli::MessageKind::warning,
                "cleanup_cancelled",
                "Partial cleanup cancelled."
            );
            return exit_code::failure;
        }
    }

    inspection.items.reset();
    std::unique_ptr<storage::ItemStore> writable;
    if (invalid_records != 0) {
        writable =
            runtime_factory.create_item_store(config, inspection.identity);
        writable->open();
    }
    std::size_t removed_files = 0;
    std::size_t removed_records = 0;
    std::size_t failed = 0;
    std::uint64_t removed_bytes = 0;
    for (const auto& candidate : candidates) {
        try {
            const auto file =
                cleanup_file(candidate.path, safe_root ? &*safe_root : nullptr);
            if (file) {
                if (!candidate.file ||
                    !same_cleanup_file(*candidate.file, *file)) {
                    throw std::runtime_error(
                        "file changed after preview; run cleanup again"
                    );
                }
                if (safe_root->remove(
                        candidate.path,
                        sync::detail::FilesystemItemKind::file,
                        util::filesystem_identity(*file)
                    )) {
                    ++removed_files;
                    removed_bytes = checked_add(
                        removed_bytes, static_cast<std::uint64_t>(file->st_size)
                    );
                }
            }
            if (candidate.remote_id) {
                writable->remove_partial_download(
                    inspection.identity.drive_id, *candidate.remote_id
                );
                ++removed_records;
            }
        } catch (const std::runtime_error& error) {
            ++failed;
            console.message(
                cli::MessageKind::error,
                "cleanup_failed",
                "Could not finish cleanup for '" + candidate.path.string() +
                    "': " + error.what()
            );
        }
    }
    console.section(
        "cleanup_result",
        "Partial cleanup result:",
        {
            {.label = "removed records:",
             .key = "removed_records",
             .value = std::to_string(removed_records)},
            {.label = "removed files:",
             .key = "removed_files",
             .value = std::to_string(removed_files)},
            {.label = "removed file bytes:",
             .key = "removed_bytes",
             .value = std::to_string(removed_bytes)},
            {.label = "retained:",
             .key = "retained",
             .value = std::to_string(retained)},
            {.label = "skipped:",
             .key = "skipped",
             .value = std::to_string(skipped)},
            {.label = "failed:",
             .key = "failed",
             .value = std::to_string(failed)},
        }
    );
    const bool complete = skipped == 0 && failed == 0;
    console.message(
        complete ? cli::MessageKind::information : cli::MessageKind::warning,
        complete ? "cleanup_completed" : "cleanup_incomplete",
        std::format(
            "Removed {} invalid partial records and {} temporary files; {} "
            "skipped, {} failed.",
            removed_records,
            removed_files,
            skipped,
            failed
        )
    );
    return complete ? exit_code::success : exit_code::failure;
}

} // namespace onedrive::app::detail
