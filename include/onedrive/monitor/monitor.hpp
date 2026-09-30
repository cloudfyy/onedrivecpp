#pragma once

#include <filesystem>

namespace onedrive::monitor {

class Monitor {
public:
    explicit Monitor(std::filesystem::path root);
    [[nodiscard]] int run() const;

private:
    std::filesystem::path root_;
};

}  // namespace onedrive::monitor
