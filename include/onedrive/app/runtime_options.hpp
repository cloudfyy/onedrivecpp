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
        .download_chunk_threshold_bytes =
            config.download_chunk_threshold_bytes,
    };
}

}  // namespace onedrive::app
