#include "onedrive/monitor/monitor.hpp"

#include <iostream>
#include <utility>

namespace onedrive::monitor {

Monitor::Monitor(std::filesystem::path root) : root_{std::move(root)} {}

int Monitor::run() const {
    std::cout << "Monitor scaffold ready for: " << root_ << '\n'
              << "Filesystem event integration is planned for the next milestone.\n";
    return 0;
}

}  // namespace onedrive::monitor
