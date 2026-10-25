#pragma once

#include <string_view>

namespace onedrive::account {

enum class DriveKind {
    unknown,
    personal,
    business,
    sharepoint_library,
};

struct DriveSourcePolicy {
    DriveKind kind{DriveKind::unknown};
    std::string_view canonical_type{"unknown"};
    std::string_view default_drive_name{"OneDrive"};
    bool supports_post_upload_enrichment{false};
    bool is_shared_library{false};
};

[[nodiscard]] constexpr DriveKind
parse_drive_kind(std::string_view raw_drive_type) noexcept {
    if (raw_drive_type == "personal") {
        return DriveKind::personal;
    }
    if (raw_drive_type == "business") {
        return DriveKind::business;
    }
    if (raw_drive_type == "documentLibrary") {
        return DriveKind::sharepoint_library;
    }
    return DriveKind::unknown;
}

[[nodiscard]] constexpr DriveSourcePolicy
drive_source_policy(DriveKind kind) noexcept {
    switch (kind) {
    case DriveKind::personal:
        return {
            .kind = DriveKind::personal,
            .canonical_type = "personal",
            .default_drive_name = "OneDrive",
            .supports_post_upload_enrichment = false,
            .is_shared_library = false,
        };
    case DriveKind::business:
        return {
            .kind = DriveKind::business,
            .canonical_type = "business",
            .default_drive_name = "OneDrive",
            .supports_post_upload_enrichment = true,
            .is_shared_library = false,
        };
    case DriveKind::sharepoint_library:
        return {
            .kind = DriveKind::sharepoint_library,
            .canonical_type = "documentLibrary",
            .default_drive_name = "Documents",
            .supports_post_upload_enrichment = true,
            .is_shared_library = true,
        };
    case DriveKind::unknown:
        break;
    }
    return {};
}

[[nodiscard]] constexpr DriveSourcePolicy
drive_source_policy(std::string_view raw_drive_type) noexcept {
    return drive_source_policy(parse_drive_kind(raw_drive_type));
}

[[nodiscard]] constexpr bool
is_default_drive_reference(std::string_view reference) noexcept {
    return reference.empty() || reference == "me";
}

} // namespace onedrive::account
