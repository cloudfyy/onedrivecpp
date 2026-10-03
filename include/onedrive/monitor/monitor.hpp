#pragma once

#include <filesystem>

namespace onedrive::monitor {

class FileMonitor {
public:
    FileMonitor() = default;
    virtual ~FileMonitor() = default;
    FileMonitor(const FileMonitor&) = delete;
    FileMonitor& operator=(const FileMonitor&) = delete;
    FileMonitor(FileMonitor&&) = delete;
    FileMonitor& operator=(FileMonitor&&) = delete;
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
