#include "ui/cli/format.hpp"
#include "ui/cli/localization.hpp"
#include "onedrive/util/byte_size.hpp"

#include <fmt/format.h>

#include <set>

namespace onedrive::cli::detail {

std::string format_file_operation(
    const events::FileOperationEvent& operation, config::Language language
) {
    auto path = fmt::format("{:?}", operation.path);
    if (!operation.previous_path.empty()) {
        path = fmt::format("{:?} -> {}", operation.previous_path, path);
    }
    const auto local_path = operation.local_path.empty()
                                ? std::string{}
                                : fmt::format("{:?}", operation.local_path);
    return format_file_operation_line(
        path,
        operation.action,
        operation.directory,
        operation.recovered,
        local_path,
        events::format_file_operation_time(operation.completed_at),
        language
    );
}

std::string format_file_history_status(
    const events::FileHistory& history, config::Language language
) {
    std::set<std::string> drives;
    for (const auto& operation : history.entries()) {
        if (!operation.drive_id.empty()) {
            drives.insert(operation.drive_id);
        }
    }
    std::string drives_csv;
    bool first = true;
    for (const auto& drive : drives) {
        drives_csv += fmt::format("{}{:?}", first ? "" : ", ", drive);
        first = false;
    }
    return format_file_history_summary(
        history.entries().size(),
        history.limit(),
        history.omitted(),
        drives_csv,
        language
    );
}

std::string format_bytes(std::uint64_t bytes) {
    const auto size = util::scale_bytes(bytes, util::ByteUnit::gibibytes);
    return size.unit == "B" ? fmt::format("{} {}", bytes, size.unit)
                            : fmt::format("{:.1f} {}", size.value, size.unit);
}

std::string format_duration(std::uint64_t seconds) {
    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    const auto remaining_seconds = seconds % 60;
    return fmt::format("{:02}:{:02}:{:02}", hours, minutes, remaining_seconds);
}

} // namespace onedrive::cli::detail
