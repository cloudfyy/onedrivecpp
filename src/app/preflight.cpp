#include "preflight.hpp"

#include "onedrive/account/account_state.hpp"
#include "checks.hpp"
#include "lock.hpp"

#include <stdexcept>

namespace onedrive::app::detail {

RuntimePreflight::RuntimePreflight(
    const config::Config& config,
    Operation operation
) {
    if (operation == Operation::authenticate ||
        operation == Operation::drives ||
        operation == Operation::shared ||
        operation == Operation::sites ||
        operation == Operation::quota ||
        operation == Operation::status ||
        operation == Operation::download ||
        operation == Operation::synchronize) {
        validate_authentication_config(config);
    }
    if ((operation == Operation::shared ||
         operation == Operation::quota ||
         operation == Operation::status ||
         operation == Operation::reset_state ||
         operation == Operation::download ||
         operation == Operation::synchronize) &&
        config.drive_id.empty()) {
        throw std::runtime_error("sync.drive_id must not be empty");
    }
    if (operation == Operation::sites &&
        !config::has_auth_scope(config.auth_scope, "Sites.Read.All") &&
        !config::has_auth_scope(config.auth_scope, "Sites.ReadWrite.All")) {
        throw std::runtime_error(
            "sites requires Sites.Read.All or Sites.ReadWrite.All in "
            "auth.scopes; run 'onedrive-cpp auth' after changing scopes"
        );
    }

    secure_state_directory(config.state_directory);
    lock_descriptor_ = acquire_runtime_lock(config.state_directory);

    validate_private_file(
        config.state_directory / "active_account",
        "active account marker",
        false
    );
    const bool authentication_required =
        operation == Operation::drives ||
        operation == Operation::shared ||
        operation == Operation::sites ||
        operation == Operation::quota ||
        operation == Operation::status ||
        operation == Operation::reset_state ||
        operation == Operation::download ||
        operation == Operation::synchronize;
    const auto token_directory =
        account::AccountState::find_active_token_directory(
            config.state_directory
        );
    if (authentication_required && !token_directory) {
        throw std::runtime_error(
            "active Microsoft account is missing; run 'onedrive-cpp auth' "
            "to initialize the account-based state layout"
        );
    }
    if (token_directory) {
        validate_private_file(
            *token_directory / "refresh_token",
            "refresh token",
            authentication_required
        );
    }
    if (operation == Operation::download ||
        operation == Operation::synchronize ||
        operation == Operation::monitor) {
        prepare_sync_directory(config, operation);
    }
}

}  // namespace onedrive::app::detail
