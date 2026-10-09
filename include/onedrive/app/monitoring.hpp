#pragma once

#include "onedrive/config/config.hpp"

#include <stop_token>

namespace onedrive::events {
class Observer;
}

namespace onedrive::app {

class RuntimeFactory;

[[nodiscard]] int monitor_account(
    config::Config config,
    const RuntimeFactory& runtime_factory,
    const events::Observer& observer,
    bool keyboard_exit,
    std::stop_token stop_token = {}
);

}  // namespace onedrive::app
