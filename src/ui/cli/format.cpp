#include "ui/cli/format.hpp"
#include "onedrive/util/byte_size.hpp"

#include <fmt/format.h>

namespace onedrive::cli::detail {

std::string format_file_operation(const events::FileOperationEvent& operation) {
    auto path = fmt::format("{:?}", operation.path);
    if (!operation.previous_path.empty()) {
        path = fmt::format("{:?} -> {}", operation.previous_path, path);
    }
    return fmt::format(
        "{}  {:<18} {}{}{}{}{}",
        events::format_file_operation_time(operation.completed_at),
        events::file_action_name(operation.action),
        operation.drive_id.empty() ? ""
                                   : fmt::format("[{}] ", operation.drive_id),
        path,
        operation.directory ? " (folder)" : "",
        operation.recovered ? " (recovered)" : "",
        operation.local_path.empty()
            ? ""
            : fmt::format(" [local: {:?}]", operation.local_path)
    );
}

std::string format_file_history_status(const events::FileHistory& history) {
    return fmt::format(
        "File history: {} retained (limit {}); {} older operations omitted.",
        history.entries().size(),
        history.limit(),
        history.omitted()
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
