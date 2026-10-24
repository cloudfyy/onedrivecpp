#include "ui/cli/format.hpp"
#include "onedrive/util/byte_size.hpp"

#include <fmt/format.h>

#include <set>

namespace onedrive::cli::detail {

std::string format_file_operation(const events::FileOperationEvent& operation) {
    auto path = fmt::format("{:?}", operation.path);
    if (!operation.previous_path.empty()) {
        path = fmt::format("{:?} -> {}", operation.previous_path, path);
    }
    return fmt::format(
        "{}  {}{}{}{}  {}",
        path,
        events::file_action_name(operation.action),
        operation.directory ? " (folder)" : "",
        operation.recovered ? " (recovered)" : "",
        operation.local_path.empty()
            ? ""
            : fmt::format(" [local: {:?}]", operation.local_path),
        events::format_file_operation_time(operation.completed_at)
    );
}

std::string format_file_history_status(const events::FileHistory& history) {
    auto status = fmt::format(
        "File history: {} retained (limit {}); {} older operations omitted.",
        history.entries().size(),
        history.limit(),
        history.omitted()
    );
    std::set<std::string> drives;
    for (const auto& operation : history.entries()) {
        if (!operation.drive_id.empty()) {
            drives.insert(operation.drive_id);
        }
    }
    if (!drives.empty()) {
        status += "\nDrives (retained history): ";
        bool first = true;
        for (const auto& drive : drives) {
            status += fmt::format("{}{:?}", first ? "" : ", ", drive);
            first = false;
        }
    }
    return status;
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
