#include "drive_fields.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <string_view>

namespace onedrive::app::detail {

std::string format_bytes(std::uint64_t bytes) {
    constexpr std::uint64_t unit = 1024;
    constexpr std::array<std::string_view, 5> names{
        "B", "KiB", "MiB", "GiB", "TiB"
    };
    double value = static_cast<double>(bytes);
    std::size_t index = 0;
    while (value >= static_cast<double>(unit) && index + 1 < names.size()) {
        value /= static_cast<double>(unit);
        ++index;
    }
    return index == 0 ? std::format("{} {}", bytes, names[index])
                      : std::format("{:.2f} {}", value, names[index]);
}

std::vector<cli::Field>
quota_fields(const std::optional<graph::DriveQuota>& quota) {
    const auto value = [&](std::uint64_t graph::DriveQuota::* member) {
        return quota ? format_bytes((*quota).*member) : "unavailable";
    };
    return {
        {.label = "total:",
         .key = "total",
         .value = value(&graph::DriveQuota::total)},
        {.label = "used:",
         .key = "used",
         .value = value(&graph::DriveQuota::used)},
        {.label = "remaining:",
         .key = "remaining",
         .value = value(&graph::DriveQuota::remaining)},
        {.label = "deleted:",
         .key = "deleted",
         .value = value(&graph::DriveQuota::deleted)},
        {.label = "quota state:",
         .key = "state",
         .value = quota ? quota->state : "unavailable"},
    };
}

std::vector<cli::Field> drive_fields(
    const graph::DriveInfo& drive,
    std::optional<bool> configured
) {
    std::vector<cli::Field> fields{
        {.label = "name:", .key = "name", .value = drive.name},
        {.label = "id:", .key = "id", .value = drive.id},
        {.label = "type:", .key = "type", .value = drive.type},
        {.label = "owner:", .key = "owner", .value = drive.owner},
    };
    if (configured) {
        fields.push_back({
            .label = "configured:",
            .key = "configured",
            .value = *configured ? "true" : "false",
        });
    }
    fields.push_back({
        .label = "web URL:",
        .key = "web_url",
        .value = drive.web_url,
    });
    std::ranges::move(quota_fields(drive.quota), std::back_inserter(fields));
    return fields;
}

} // namespace onedrive::app::detail
