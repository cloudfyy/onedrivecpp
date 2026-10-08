#pragma once

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
};

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
            return {};
        case Operation::diagnose:
            return {.supports_tui = true};
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
