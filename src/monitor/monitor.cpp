#include "onedrive/monitor/monitor.hpp"

#include <utility>

namespace onedrive::monitor {

Monitor::Monitor(std::filesystem::path root) : root_{std::move(root)} {}

int Monitor::run() const {
    return 0;
}

}  // namespace onedrive::monitor
