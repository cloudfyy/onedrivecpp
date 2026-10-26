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
    const bool chinese = is_chinese(language);
    return fmt::format(
        "{}  {}{}{}{}  {}",
        path,
        translate_file_action(operation.action, language),
        operation.directory ? (chinese ? "（文件夹）" : " (folder)") : "",
        operation.recovered ? (chinese ? "（已恢复）" : " (recovered)") : "",
        operation.local_path.empty()
            ? ""
            : (chinese ? fmt::format(" [本地：{:?}]", operation.local_path)
                       : fmt::format(" [local: {:?}]", operation.local_path)),
        events::format_file_operation_time(operation.completed_at)
    );
}

std::string format_file_history_status(
    const events::FileHistory& history, config::Language language
) {
    const bool chinese = is_chinese(language);
    auto status =
        chinese
            ? fmt::format(
                  "文件历史记录：保留 {} 条（上限 {}）；已省略 {} 条更早的操作。",
                  history.entries().size(),
                  history.limit(),
                  history.omitted()
              )
            : fmt::format(
                  "File history: {} retained (limit {}); {} older operations "
                  "omitted.",
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
        status += chinese ? "\n驱动器（保留的历史记录）："
                          : "\nDrives (retained history): ";
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
