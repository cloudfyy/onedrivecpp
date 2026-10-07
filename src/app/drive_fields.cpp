#include "drive_fields.hpp"

namespace onedrive::app::detail {

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
    return fields;
}

} // namespace onedrive::app::detail
