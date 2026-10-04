#pragma once

#include "onedrive/auth/device_auth.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"

namespace onedrive::app {

inline auth::DeviceAuthOptions device_auth_options(
    const config::Config& config
) {
    return {
        .application_id = config.application_id,
        .tenant_id = config.azure_tenant_id,
        .auth_endpoint = config.auth_endpoint,
        .scope = config.auth_scope,
    };
}

inline graph::GraphOptions graph_options(const config::Config& config) {
    return {
        .drive_id = config.drive_id,
        .endpoint = config.graph_endpoint,
        .maximum_throttle_retries =
            config.graph_maximum_throttle_retries,
        .initial_throttle_delay = config.graph_initial_throttle_delay,
        .maximum_throttle_delay = config.graph_maximum_throttle_delay,
        .download_maximum_retries = config.download_maximum_retries,
        .download_chunk_threshold_bytes =
            config.download_chunk_threshold_bytes,
        .download_checkpoint_interval_bytes =
            config.download_checkpoint_interval_bytes,
        .download_transport = {
            .transfer = config.transfer_transport,
            .maximum_receive_speed_bytes_per_second =
                config.download_maximum_rate_bytes_per_second,
        },
        .relaxed_download_validation =
            config.download_validation ==
                config::DownloadValidationMode::relaxed,
        .private_download_permissions =
            config.sync_permissions ==
                config::SyncPermissionsMode::private_access,
    };
}

}  // namespace onedrive::app
