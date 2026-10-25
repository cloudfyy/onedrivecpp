#pragma once

#include "onedrive/config/config.hpp"

#include <stop_token>

namespace onedrive::events {
class Observer;
}

namespace onedrive::sync {
class PreviewCache;
}

namespace onedrive::app {

class RuntimeFactory;

[[nodiscard]] int synchronize_account(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const events::Observer& observer,
    const std::stop_token& stop_token = {},
    sync::PreviewCache* preview_cache = nullptr
);

}  // namespace onedrive::app
