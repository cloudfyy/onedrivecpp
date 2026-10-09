#pragma once

#include "onedrive/ui/common/event.hpp"

#include <mutex>
#include <string_view>

namespace onedrive::events {

class Observer {
public:
    virtual ~Observer() = default;
    Observer(const Observer&) = delete;
    Observer& operator=(const Observer&) = delete;
    Observer(Observer&&) = delete;
    Observer& operator=(Observer&&) = delete;

    void message(
        MessageKind kind, std::string_view event, std::string_view text
    ) const;
    void section(
        std::string_view event,
        std::string_view title,
        const std::vector<Field>& fields
    ) const;
    void delta_progress(
        std::size_t pages, std::size_t items, util::ProgressState state
    ) const;
    void delta_summary(const DeltaSummary& summary) const;
    void blocked_item(
        std::string_view path,
        std::string_view reason_code,
        std::string_view reason_message
    ) const;
    void download_progress(
        std::size_t completed_files,
        std::size_t file_count,
        std::uint64_t downloaded,
        std::uint64_t total,
        util::ProgressState state,
        const DownloadProgressMetrics& metrics = {}
    ) const;
    void end_download_progress() const;
    void operation_state(
        OperationKind operation, OperationState state
    ) const;

protected:
    Observer() = default;

    [[nodiscard]] std::scoped_lock<std::mutex> lock() const;

    // Called under lock(); implementations must not re-enter this observer.
    virtual void on_event(const Event& event) const = 0;

private:
    mutable std::mutex mutex_;
};

} // namespace onedrive::events
