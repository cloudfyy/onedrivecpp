#pragma once

#include "onedrive/logging/logging.hpp"
#include "onedrive/util/progress.hpp"
#include "onedrive/sync/capabilities.hpp"

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace onedrive::events {

using MessageKind = logging::Severity;

struct Field {
    std::string label;
    std::string key;
    std::string value;
};

struct TransferProgressMetrics {
    std::uint64_t bytes_per_second{0};
    std::optional<std::uint64_t> estimated_seconds_remaining;
    std::uint64_t elapsed_milliseconds{0};
};

using DownloadProgressMetrics = TransferProgressMetrics;
using UploadProgressMetrics = TransferProgressMetrics;

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

struct UploadProgressEvent {
    std::size_t completed_files{0};
    std::size_t file_count{0};
    std::uint64_t uploaded{0};
    std::uint64_t total{0};
    util::ProgressState state{util::ProgressState::ongoing};
    UploadProgressMetrics metrics;
};

struct EndUploadProgressEvent {};

enum class SyncPhase {
    preparing,
    scanning,
    scanning_local,
    downloading,
    uploading,
    applying,
    waiting,
};

struct SyncPhaseEvent {
    SyncPhase phase;
};

struct SyncContextEvent {
    sync::SyncMode direction{sync::SyncMode::bidirectional};
    bool preview{false};
};

[[nodiscard]] std::string_view sync_phase_name(SyncPhase phase);

struct SyncSummaryEvent {
    std::size_t downloaded{0};
    std::size_t uploaded{0};
    std::size_t moved{0};
    std::size_t deleted{0};
    std::size_t blocked{0};
};

enum class FileAction {
    downloaded,
    uploaded,
    moved_locally,
    moved_remotely,
    deleted_locally,
    deleted_remotely,
};

struct FileOperationEvent {
    std::chrono::system_clock::time_point completed_at{
        std::chrono::system_clock::now()
    };
    FileAction action{FileAction::downloaded};
    std::string path{};
    std::string previous_path{};
    std::string drive_id{};
    std::string local_path{};
    bool directory{false};
    bool recovered{false};
};

[[nodiscard]] std::string_view file_action_name(FileAction action);
[[nodiscard]] std::string
format_file_operation_time(std::chrono::system_clock::time_point timestamp);

// Operation counts accumulate; blockers describe only the latest round.
[[nodiscard]] SyncSummaryEvent accumulate_sync_summary(
    SyncSummaryEvent totals, const SyncSummaryEvent& round
) noexcept;

enum class OperationKind {
    authentication,
    synchronization,
    monitoring,
};

enum class OperationState {
    idle,
    authenticating,
    ready,
    syncing,
    watching,
    stopping,
    failed,
};

struct OperationStateEvent {
    OperationKind operation;
    OperationState state;
};

using Event = std::variant<
    MessageEvent,
    SectionEvent,
    DeltaProgressEvent,
    DeltaSummaryEvent,
    BlockedItemEvent,
    DownloadProgressEvent,
    EndDownloadProgressEvent,
    OperationStateEvent,
    UploadProgressEvent,
    EndUploadProgressEvent,
    SyncPhaseEvent,
    SyncSummaryEvent,
    SyncContextEvent,
    FileOperationEvent>;

[[nodiscard]] unsigned download_progress_percentage(
    std::size_t completed_files,
    std::size_t file_count,
    std::uint64_t downloaded,
    std::uint64_t total,
    util::ProgressState state
) noexcept;

} // namespace onedrive::events
