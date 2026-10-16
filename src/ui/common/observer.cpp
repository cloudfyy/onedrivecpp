#include "onedrive/ui/common/observer.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <stdexcept>

namespace onedrive::events {
namespace {

constexpr unsigned completed_percentage = 100;
constexpr unsigned maximum_incomplete_percentage = 99;

} // namespace

std::string_view file_action_name(FileAction action) {
    switch (action) {
    case FileAction::downloaded:
        return "downloaded";
    case FileAction::uploaded:
        return "uploaded";
    case FileAction::moved_locally:
        return "moved_locally";
    case FileAction::moved_remotely:
        return "moved_remotely";
    case FileAction::deleted_locally:
        return "deleted_locally";
    case FileAction::deleted_remotely:
        return "deleted_remotely";
    }
    throw std::logic_error{"unknown file operation action"};
}

std::string
format_file_operation_time(std::chrono::system_clock::time_point timestamp) {
    const auto seconds = std::chrono::system_clock::to_time_t(
        std::chrono::floor<std::chrono::seconds>(timestamp)
    );
    std::tm value{};
    if (::gmtime_r(&seconds, &value) == nullptr) {
        throw std::runtime_error{"cannot format file operation time"};
    }
    std::ostringstream output;
    output << std::put_time(&value, "%Y-%m-%dT%H:%M:%SZ");
    if (!output) {
        throw std::runtime_error{"cannot format file operation time"};
    }
    return output.str();
}

void Observer::file_operation(const FileOperationEvent& operation) const {
    const std::scoped_lock lock{mutex_};
    on_event(operation);
}

SyncSummaryEvent accumulate_sync_summary(
    SyncSummaryEvent totals, const SyncSummaryEvent& round
) noexcept {
    totals.downloaded += round.downloaded;
    totals.uploaded += round.uploaded;
    totals.moved += round.moved;
    totals.deleted += round.deleted;
    totals.blocked = round.blocked;
    return totals;
}

void Observer::report_message(
    MessageKind kind, std::string_view event, std::string_view text
) const {
    logging::write_event(kind, text);
    message(kind, event, text);
}

void Observer::report_blocked_item(
    std::string_view path,
    std::string_view reason_code,
    std::string_view reason_message
) const {
    logging::write_event(
        MessageKind::warning,
        std::format(
            "Blocked remote item '{}': {} ({})",
            path,
            reason_message,
            reason_code
        )
    );
    blocked_item(path, reason_code, reason_message);
}

std::string_view sync_phase_name(SyncPhase phase) {
    switch (phase) {
    case SyncPhase::preparing:
        return "preparing";
    case SyncPhase::scanning:
        return "scanning";
    case SyncPhase::scanning_local:
        return "scanning_local";
    case SyncPhase::downloading:
        return "downloading";
    case SyncPhase::uploading:
        return "uploading";
    case SyncPhase::applying:
        return "applying";
    case SyncPhase::waiting:
        return "waiting";
    }
    throw std::logic_error{"unknown synchronization phase"};
}

std::scoped_lock<std::mutex> Observer::lock() const {
    return std::scoped_lock{mutex_};
}

unsigned download_progress_percentage(
    std::size_t completed_files,
    std::size_t file_count,
    std::uint64_t downloaded,
    std::uint64_t total,
    util::ProgressState state
) noexcept {
    const bool completed = state == util::ProgressState::completed;
    auto percentage =
        total == 0
            ? (file_count == 0 ? (completed ? completed_percentage : 0U)
                               : static_cast<unsigned>(std::min(
                                     static_cast<double>(completed_percentage),
                                     std::floor(
                                         static_cast<double>(completed_files) *
                                         100.0 / static_cast<double>(file_count)
                                     )
                                 )))
            : static_cast<unsigned>(std::min(
                  static_cast<double>(completed_percentage),
                  std::floor(
                      static_cast<double>(downloaded) * 100.0 /
                      static_cast<double>(total)
                  )
              ));
    if (!completed) {
        percentage = std::min(percentage, maximum_incomplete_percentage);
    }
    return percentage;
}

void Observer::message(
    MessageKind kind, std::string_view event, std::string_view text
) const {
    const std::scoped_lock lock{mutex_};
    on_event(
        MessageEvent{
            .kind = kind,
            .event = std::string{event},
            .text = std::string{text},
        }
    );
}

void Observer::section(
    std::string_view event,
    std::string_view title,
    const std::vector<Field>& fields
) const {
    const std::scoped_lock lock{mutex_};
    on_event(
        SectionEvent{
            .event = std::string{event},
            .title = std::string{title},
            .fields = fields,
        }
    );
}

void Observer::delta_progress(
    std::size_t pages, std::size_t items, util::ProgressState state
) const {
    const std::scoped_lock lock{mutex_};
    on_event(
        DeltaProgressEvent{
            .pages = pages,
            .items = items,
            .state = state,
        }
    );
}

void Observer::delta_summary(const DeltaSummary& summary) const {
    const std::scoped_lock lock{mutex_};
    on_event(DeltaSummaryEvent{.summary = summary});
}

void Observer::blocked_item(
    std::string_view path,
    std::string_view reason_code,
    std::string_view reason_message
) const {
    const std::scoped_lock lock{mutex_};
    on_event(
        BlockedItemEvent{
            .path = std::string{path},
            .reason_code = std::string{reason_code},
            .reason_message = std::string{reason_message},
        }
    );
}

void Observer::download_progress(
    std::size_t completed_files,
    std::size_t file_count,
    std::uint64_t downloaded,
    std::uint64_t total,
    util::ProgressState state,
    const DownloadProgressMetrics& metrics
) const {
    const std::scoped_lock lock{mutex_};
    on_event(
        DownloadProgressEvent{
            .completed_files = completed_files,
            .file_count = file_count,
            .downloaded = downloaded,
            .total = total,
            .state = state,
            .metrics = metrics,
        }
    );
}

void Observer::end_download_progress() const {
    const std::scoped_lock lock{mutex_};
    on_event(EndDownloadProgressEvent{});
}

void Observer::upload_progress(const UploadProgressEvent& progress) const {
    const std::scoped_lock lock{mutex_};
    on_event(progress);
}

void Observer::end_upload_progress() const {
    const std::scoped_lock lock{mutex_};
    on_event(EndUploadProgressEvent{});
}

void Observer::sync_phase(SyncPhase phase) const {
    const std::scoped_lock lock{mutex_};
    on_event(SyncPhaseEvent{phase});
}

void Observer::sync_context(sync::SyncMode direction, bool preview) const {
    const std::scoped_lock lock{mutex_};
    on_event(SyncContextEvent{direction, preview});
}

void Observer::sync_summary(const SyncSummaryEvent& summary) const {
    const std::scoped_lock lock{mutex_};
    on_event(summary);
}

void Observer::operation_state(
    OperationKind operation, OperationState state
) const {
    const std::scoped_lock lock{mutex_};
    on_event(OperationStateEvent{.operation = operation, .state = state});
}

} // namespace onedrive::events
