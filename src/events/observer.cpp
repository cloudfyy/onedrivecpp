#include "onedrive/events/observer.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace onedrive::events {
namespace {

constexpr unsigned completed_percentage = 100;
constexpr unsigned maximum_incomplete_percentage = 99;

} // namespace

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

} // namespace onedrive::events
