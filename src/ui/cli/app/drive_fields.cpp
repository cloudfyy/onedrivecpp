#include "drive_fields.hpp"
#include "onedrive/util/byte_size.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <string_view>

namespace onedrive::app::detail {

std::vector<cli::Field> account_fields(const account::DriveIdentity& identity) {
    return {
        {.label = "account:",
         .key = "account",
         .value = identity.user_display_name},
        {.label = "drive:", .key = "drive", .value = identity.drive_name},
        {.label = "type:",
         .key = "drive_type",
         .value = identity.drive_type.empty()
                      ? std::string{identity.source_policy().canonical_type}
                      : identity.drive_type},
    };
}

std::string format_bytes(std::uint64_t bytes) {
    const auto size = util::scale_bytes(bytes, util::ByteUnit::tebibytes);
    return size.unit == "B" ? std::format("{} {}", bytes, size.unit)
                            : std::format("{:.2f} {}", size.value, size.unit);
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

std::vector<cli::Field>
drive_fields(const graph::DriveInfo& drive, std::optional<bool> configured) {
    std::vector<cli::Field> fields{
        {.label = "name:", .key = "name", .value = drive.name},
        {.label = "drive id:", .key = "drive_id", .value = drive.id},
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
