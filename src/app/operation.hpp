#pragma once

#include <string_view>

namespace onedrive::app::detail {

enum class Operation {
    authenticate,
    logout,
    diagnose,
    drives,
    shared,
    sites,
    quota,
    status,
    blocked,
    storage,
    partials,
    files,
    verify,
    config,
    reset_cursor,
    cleanup_state,
    migrate_state,
    clear_state,
    download,
    monitor,
    synchronize,
    monitor_status,
    monitor_trigger,
    monitor_stop,
};

constexpr std::string_view operation_name(Operation operation) noexcept {
    switch (operation) {
    case Operation::authenticate:
        return "authenticate";
    case Operation::logout:
        return "logout";
    case Operation::diagnose:
        return "diagnose";
    case Operation::drives:
        return "drives";
    case Operation::shared:
        return "shared";
    case Operation::sites:
        return "sites";
    case Operation::quota:
        return "quota";
    case Operation::status:
        return "status";
    case Operation::blocked:
        return "blocked";
    case Operation::storage:
        return "storage";
    case Operation::partials:
        return "partials";
    case Operation::files:
        return "files";
    case Operation::verify:
        return "verify";
    case Operation::config:
        return "config";
    case Operation::reset_cursor:
        return "reset_cursor";
    case Operation::cleanup_state:
        return "cleanup_state";
    case Operation::migrate_state:
        return "migrate_state";
    case Operation::clear_state:
        return "clear_state";
    case Operation::download:
        return "download";
    case Operation::monitor:
        return "monitor";
    case Operation::synchronize:
        return "synchronize";
    case Operation::monitor_status:
        return "monitor_status";
    case Operation::monitor_trigger:
        return "monitor_trigger";
    case Operation::monitor_stop:
        return "monitor_stop";
    }
    return "unknown";
}

struct OperationCapabilities {
    bool supports_tui{false};
    bool validates_authentication_config{false};
    bool requires_drive_id{false};
    bool requires_authentication{false};
    bool requires_sync_directory{false};
};

constexpr OperationCapabilities operation_capabilities(
    Operation operation
) noexcept {
    switch (operation) {
        case Operation::authenticate:
            return {
                .supports_tui = true,
                .validates_authentication_config = true,
            };
        case Operation::logout:
        case Operation::migrate_state:
        case Operation::monitor_status:
        case Operation::monitor_trigger:
        case Operation::monitor_stop:
            return {};
        case Operation::diagnose:
            return {.supports_tui = true};
        case Operation::blocked:
            return {.supports_tui = true, .requires_drive_id = true};
        case Operation::drives:
            return {
                .supports_tui = true,
                .validates_authentication_config = true,
                .requires_authentication = true,
            };
        case Operation::shared:
            return {
                .supports_tui = true,
                .validates_authentication_config = true,
                .requires_drive_id = true,
                .requires_authentication = true,
            };
        case Operation::sites:
            return {
                .supports_tui = true,
                .validates_authentication_config = true,
                .requires_authentication = true,
            };
        case Operation::quota:
            return {
                .supports_tui = true,
                .validates_authentication_config = true,
                .requires_drive_id = true,
                .requires_authentication = true,
            };
        case Operation::status:
        case Operation::storage:
        case Operation::partials:
        case Operation::files:
        case Operation::verify:
            return {
                .supports_tui = true,
                .validates_authentication_config = true,
                .requires_drive_id = true,
                .requires_authentication = true,
                .requires_sync_directory =
                    operation == Operation::storage ||
                    operation == Operation::partials ||
                    operation == Operation::files,
            };
        case Operation::config:
            return {.supports_tui = true};
        case Operation::reset_cursor:
        case Operation::cleanup_state:
        case Operation::clear_state:
            return {
                .requires_drive_id = true,
                .requires_authentication = true,
                .requires_sync_directory =
                    operation == Operation::cleanup_state,
            };
        case Operation::download:
        case Operation::monitor:
        case Operation::synchronize:
            return {
                .supports_tui = true,
                .validates_authentication_config = true,
                .requires_drive_id = true,
                .requires_authentication = true,
                .requires_sync_directory = true,
            };
    }
    return {};
}

} // namespace onedrive::app::detail
