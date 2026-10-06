#include "onedrive/monitor/monitor.hpp"
#include "monitor/signal.hpp"
#include "monitor/state.hpp"
#include "support/common.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace onedrive::test::monitor {

using namespace std::chrono_literals;

using onedrive::test::fail;
using onedrive::test::wait_until;

struct MonitorFixture final {
    onedrive::test::TemporaryDirectory temporary;
    std::filesystem::path root{temporary.path() / "sync"};

    MonitorFixture() {
        std::filesystem::create_directory(root);
    }
};

} // namespace onedrive::test::monitor
