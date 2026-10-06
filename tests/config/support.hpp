#pragma once

#include "onedrive/app/runtime_options.hpp"
#include "onedrive/config/config.hpp"
#include "test_support.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace onedrive::test::config {

struct ConfigFixture final {
    TemporaryDirectory temporary;
    std::filesystem::path path{temporary.path() / "config.toml"};
};

} // namespace onedrive::test::config
