#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace onedrive::monitor {

enum class ControlCommand { status, trigger, stop };

struct ControlReply {
    std::string acknowledgement;
    std::string state;
    std::int64_t pid{0};
    std::filesystem::path sync_directory;
    bool pending{false};
    std::uint64_t completed_rounds{0};
    std::optional<int> last_result;
};

[[nodiscard]] ControlReply request_control(
    const std::filesystem::path& state_directory, ControlCommand command
);

class ControlServer final {
public:
    // The owner must hold its state-directory runtime lock for this lifetime.
    ControlServer(
        const std::filesystem::path& state_directory,
        const std::filesystem::path& sync_directory
    );
    ~ControlServer();
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    [[nodiscard]] int descriptor() const noexcept;
    [[nodiscard]] std::stop_token stop_token() const noexcept;
    void request_stop();
    [[nodiscard]] bool take_trigger();
    void begin_round();
    void finish_round(int result);
    void check_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace onedrive::monitor
