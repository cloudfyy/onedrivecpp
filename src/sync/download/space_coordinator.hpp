#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stdexcept>

namespace onedrive::sync::detail {

[[nodiscard]] std::uintmax_t download_safety_reserve(
    std::uintmax_t transfer_bytes
);

class DownloadSpaceCancelledError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class DownloadSpaceCoordinator final {
public:
    using SpaceQuery =
        std::function<std::uintmax_t(const std::filesystem::path&)>;

    class Lease final {
    public:
        Lease() = default;
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;

        void expand(std::uintmax_t bytes);
        void consume(std::uintmax_t bytes);
        [[nodiscard]] std::uintmax_t remaining() const noexcept;

    private:
        friend DownloadSpaceCoordinator;

        Lease(
            DownloadSpaceCoordinator* owner,
            std::uintmax_t remaining
        ) noexcept;
        void release() noexcept;

        DownloadSpaceCoordinator* owner_{nullptr};
        std::uintmax_t remaining_{0};
    };

    DownloadSpaceCoordinator(
        std::filesystem::path directory,
        std::uintmax_t safety_reserve,
        SpaceQuery query = {}
    );

    [[nodiscard]] Lease acquire(std::uintmax_t bytes);
    void cancel() noexcept;

private:
    friend Lease;

    enum class ReservationKind {
        initial,
        expansion,
    };

    void reserve(std::uintmax_t bytes, ReservationKind kind);
    void consume(std::uintmax_t bytes);
    void release(std::uintmax_t remaining) noexcept;

    std::filesystem::path directory_;
    std::uintmax_t safety_reserve_;
    SpaceQuery query_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::uintmax_t reserved_{0};
    std::size_t active_leases_{0};
    bool cancelled_{false};
};

}  // namespace onedrive::sync::detail
