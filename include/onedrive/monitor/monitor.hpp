#pragma once

#include <filesystem>

namespace onedrive::monitor {

class FileMonitor {
public:
    virtual ~FileMonitor() = default;
    [[nodiscard]] virtual int run() const = 0;
};

class Monitor final : public FileMonitor {
public:
    explicit Monitor(std::filesystem::path root);
    [[nodiscard]] int run() const override;

private:
    std::filesystem::path root_;
};

}  // namespace onedrive::monitor
