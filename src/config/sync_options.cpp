#include "config/parse.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace onedrive::config::detail {

void load_sync_options(Config& config, const toml::table& root, const std::filesystem::path& config_path) {
    if (const auto* sync = optional_table(root, "sync", "sync")) {
        validate_keys(
            *sync,
            {
                "directory",
                "drive_id",
                "dry_run",
                "permissions",
                "sync_list",
                "sync_root_files",
                "local_conflict",
                "maximum_remote_deletions",
                "mount_point",
                "upload",
            },
            "sync"
        );
        if (const auto value = optional_value<std::string>(
                *sync,
                "local_conflict",
                "sync.local_conflict",
                "a string"
            )) {
            config.local_conflict = parse_local_conflict(*value);
        }
        if (sync->contains("maximum_remote_deletions")) {
            config.maximum_remote_deletions = size_value(
                *sync,
                "maximum_remote_deletions",
                "sync.maximum_remote_deletions"
            );
        }
        if (const auto value = optional_value<std::string>(
                *sync,
                "directory",
                "sync.directory",
                "a string"
            )) {
            config.sync_directory = *value;
        }
        if (const auto value = optional_value<std::string>(
                *sync,
                "mount_point",
                "sync.mount_point",
                "a string"
            )) {
            if (value->empty()) {
                throw std::runtime_error(
                    "sync.mount_point must not be empty"
                );
            }
            config.sync_mount_point = *value;
        }
        if (const auto value = optional_value<std::string>(
                *sync,
                "drive_id",
                "sync.drive_id",
                "a string"
            )) {
            config.drive_id = *value;
        }
        if (const auto value = optional_value<bool>(
                *sync,
                "upload",
                "sync.upload",
                "a boolean"
            )) {
            config.upload = *value;
        }
        if (const auto value = optional_value<bool>(
                *sync,
                "dry_run",
                "sync.dry_run",
                "a boolean"
            )) {
            config.dry_run = *value;
        }
        if (const auto value = optional_value<std::string>(
                *sync,
                "sync_list",
                "sync.sync_list",
                "a string"
            )) {
            if (value->empty()) {
                throw std::runtime_error(
                    "sync.sync_list must not be empty"
                );
            }
            auto sync_list = std::filesystem::path{*value};
            if (sync_list.is_relative()) {
                sync_list = config_path.parent_path() / sync_list;
            }
            config.sync_list = sync_list.lexically_normal();
        }
        if (const auto value = optional_value<bool>(
                *sync,
                "sync_root_files",
                "sync.sync_root_files",
                "a boolean"
            )) {
            config.sync_root_files = *value;
        }
        if (const auto value = optional_value<std::string>(
                *sync,
                "permissions",
                "sync.permissions",
                "a string"
            )) {
            config.sync_permissions = parse_sync_permissions(*value);
        }
    }

}

void load_download_options(Config& config, const toml::table& root) {
    if (const auto* download =
            optional_table(root, "download", "download")) {
        validate_keys(
            *download,
            {
                "concurrency",
                "maximum_retries",
                "chunk_threshold_bytes",
                "checkpoint_interval_bytes",
                "maximum_rate_bytes_per_second",
                "maximum_total_rate_bytes_per_second",
                "validation",
            },
            "download"
        );
        if (download->contains("concurrency")) {
            const auto concurrency = unsigned_value(
                *download,
                "concurrency",
                "download.concurrency"
            );
            if (concurrency < 1 || concurrency > 16) {
                throw std::runtime_error(
                    "download.concurrency must be between 1 and 16"
                );
            }
            config.download_concurrency =
                static_cast<std::size_t>(concurrency);
        }
        if (download->contains("maximum_retries")) {
            config.download_maximum_retries = size_value(
                *download,
                "maximum_retries",
                "download.maximum_retries"
            );
        }
        if (download->contains("chunk_threshold_bytes")) {
            const auto threshold = unsigned_value(
                *download,
                "chunk_threshold_bytes",
                "download.chunk_threshold_bytes"
            );
            if (threshold == 0) {
                throw std::runtime_error(
                    "download.chunk_threshold_bytes must be greater than 0"
                );
            }
            config.download_chunk_threshold_bytes = threshold;
        }
        if (download->contains("checkpoint_interval_bytes")) {
            const auto interval = unsigned_value(
                *download,
                "checkpoint_interval_bytes",
                "download.checkpoint_interval_bytes"
            );
            if (interval == 0) {
                throw std::runtime_error(
                    "download.checkpoint_interval_bytes must be greater than 0"
                );
            }
            config.download_checkpoint_interval_bytes = interval;
        }
        if (download->contains("maximum_rate_bytes_per_second")) {
            config.download_maximum_rate_bytes_per_second =
                unsigned_value(
                    *download,
                    "maximum_rate_bytes_per_second",
                    "download.maximum_rate_bytes_per_second"
                );
        }
        if (download->contains(
                "maximum_total_rate_bytes_per_second"
            )) {
            config.download_maximum_total_rate_bytes_per_second =
                unsigned_value(
                    *download,
                    "maximum_total_rate_bytes_per_second",
                    "download.maximum_total_rate_bytes_per_second"
                );
        }
        if (const auto value = optional_value<std::string>(
                *download,
                "validation",
                "download.validation",
                "a string"
            )) {
            config.download_validation =
                parse_download_validation(*value);
        }
    }

}

void load_upload_options(Config& config, const toml::table& root) {
    if (const auto* upload = optional_table(root, "upload", "upload")) {
        validate_keys(
            *upload,
            {
                "concurrency",
                "chunk_size_bytes",
                "maximum_rate_bytes_per_second",
                "maximum_total_rate_bytes_per_second",
            },
            "upload"
        );
        if (upload->contains("concurrency")) {
            const auto concurrency = unsigned_value(
                *upload,
                "concurrency",
                "upload.concurrency"
            );
            if (concurrency < 1 || concurrency > 16) {
                throw std::runtime_error(
                    "upload.concurrency must be between 1 and 16"
                );
            }
            config.upload_concurrency =
                static_cast<std::size_t>(concurrency);
        }
        if (upload->contains("chunk_size_bytes")) {
            const auto chunk_size = unsigned_value(
                *upload, "chunk_size_bytes", "upload.chunk_size_bytes"
            );
            constexpr std::uint64_t upload_quantum = std::uint64_t{320} * 1024U;
            constexpr std::uint64_t maximum_chunk_size =
                std::uint64_t{60} * 1024U * 1024U;
            if (chunk_size == 0 || chunk_size % upload_quantum != 0 ||
                chunk_size >= maximum_chunk_size) {
                throw std::runtime_error(
                    "upload.chunk_size_bytes must be a positive multiple of "
                    "320 KiB and less than 60 MiB"
                );
            }
            config.upload_chunk_size_bytes = chunk_size;
        }
        if (upload->contains("maximum_rate_bytes_per_second")) {
            config.upload_maximum_rate_bytes_per_second = unsigned_value(
                *upload,
                "maximum_rate_bytes_per_second",
                "upload.maximum_rate_bytes_per_second"
            );
        }
        if (upload->contains("maximum_total_rate_bytes_per_second")) {
            config.upload_maximum_total_rate_bytes_per_second = unsigned_value(
                *upload,
                "maximum_total_rate_bytes_per_second",
                "upload.maximum_total_rate_bytes_per_second"
            );
        }
    }

}

void load_monitor_options(Config& config, const toml::table& root) {
    if (const auto* monitor = optional_table(root, "monitor", "monitor")) {
        validate_keys(
            *monitor,
            {"poll_interval_seconds", "settle_delay_milliseconds"},
            "monitor"
        );
        if (monitor->contains("poll_interval_seconds")) {
            const auto interval = duration_value<std::chrono::seconds>(
                *monitor,
                "poll_interval_seconds",
                "monitor.poll_interval_seconds"
            );
            if (interval < std::chrono::seconds{1} ||
                interval > std::chrono::hours{24}) {
                throw std::runtime_error(
                    "monitor.poll_interval_seconds must be between 1 and 86400"
                );
            }
            config.monitor_poll_interval = interval;
        }
        if (monitor->contains("settle_delay_milliseconds")) {
            const auto delay = duration_value<std::chrono::milliseconds>(
                *monitor,
                "settle_delay_milliseconds",
                "monitor.settle_delay_milliseconds"
            );
            if (delay > std::chrono::minutes{1}) {
                throw std::runtime_error(
                    "monitor.settle_delay_milliseconds must not exceed 60000"
                );
            }
            config.monitor_settle_delay = delay;
        }
    }

}

void load_state_options(Config& config, const toml::table& root) {
    if (const auto* state = optional_table(root, "state", "state")) {
        validate_keys(*state, {"directory"}, "state");
        if (const auto value = optional_value<std::string>(
                *state,
                "directory",
                "state.directory",
                "a string"
            )) {
            config.state_directory = *value;
        }
    }

}

void load_filesystem_options(Config& config, const toml::table& root) {
    if (const auto* filesystem =
            optional_table(root, "filesystem", "filesystem")) {
        validate_keys(*filesystem, {"metadata"}, "filesystem");
        if (const auto value = optional_value<std::string>(
                *filesystem,
                "metadata",
                "filesystem.metadata",
                "a string"
            )) {
            config.filesystem_metadata = parse_filesystem_metadata(*value);
        }
    }

    if (config.graph_maximum_throttle_delay <
        config.graph_initial_throttle_delay) {
        throw std::runtime_error(
            "graph.throttle.maximum_delay_seconds must be greater than or equal "
            "to graph.throttle.initial_delay_seconds"
        );
    }
    spdlog::debug("TOML configuration loaded and validated");
}

}  // namespace onedrive::config::detail
