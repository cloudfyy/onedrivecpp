#pragma once

#include "onedrive/util/progress.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace onedrive::events {

enum class MessageKind {
    information,
    success,
    warning,
    error,
};

struct Field {
    std::string label;
    std::string key;
    std::string value;
};

struct DownloadProgressMetrics {
    std::uint64_t bytes_per_second{0};
    std::optional<std::uint64_t> estimated_seconds_remaining;
    std::uint64_t elapsed_milliseconds{0};
};

struct DeltaSummary {
    std::size_t pages{0};
    std::size_t scanned_items{0};
    std::size_t unique_changes{0};
    std::size_t files{0};
    std::size_t directories{0};
    std::size_t deletions{0};
};

struct MessageEvent {
    MessageKind kind;
    std::string event;
    std::string text;
};

struct SectionEvent {
    std::string event;
    std::string title;
    std::vector<Field> fields;
};

struct DeltaProgressEvent {
    std::size_t pages;
    std::size_t items;
    util::ProgressState state;
};

struct DeltaSummaryEvent {
    DeltaSummary summary;
};

struct BlockedItemEvent {
    std::string path;
    std::string reason_code;
    std::string reason_message;
};

struct DownloadProgressEvent {
    std::size_t completed_files{0};
    std::size_t file_count{0};
    std::uint64_t downloaded{0};
    std::uint64_t total{0};
    util::ProgressState state{util::ProgressState::ongoing};
    DownloadProgressMetrics metrics;
};

struct EndDownloadProgressEvent {};

using Event = std::variant<
    MessageEvent,
    SectionEvent,
    DeltaProgressEvent,
    DeltaSummaryEvent,
    BlockedItemEvent,
    DownloadProgressEvent,
    EndDownloadProgressEvent>;

[[nodiscard]] unsigned download_progress_percentage(
    std::size_t completed_files,
    std::size_t file_count,
    std::uint64_t downloaded,
    std::uint64_t total,
    util::ProgressState state
) noexcept;

} // namespace onedrive::events
