#pragma once

#include "onedrive/account/account_state.hpp"

#include <string>
#include <utility>

namespace onedrive::test {

[[nodiscard]] inline account::DriveIdentity test_drive_identity(
    std::string configured_drive_id = "me",
    std::string drive_id = "drive-id"
) {
    return {
        .user_id = "user-id",
        .user_display_name = "Test User",
        .configured_drive_id = std::move(configured_drive_id),
        .drive_id = std::move(drive_id),
        .drive_name = "Test Drive",
    };
}

}  // namespace onedrive::test
