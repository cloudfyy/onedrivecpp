#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
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

private:
    enum class ReservationKind {
        initial,
        expansion,
    };

    struct State {
        std::filesystem::path directory;
        std::uintmax_t safety_reserve;
        SpaceQuery query;
        std::mutex mutex;
        std::condition_variable condition;
        std::uintmax_t reserved{0};
        std::size_t active_leases{0};
        bool cancelled{false};
        bool closed{false};
    };

public:
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
            std::shared_ptr<State> state,
            std::uintmax_t remaining
        ) noexcept;
        void release() noexcept;

        std::shared_ptr<State> state_;
        std::uintmax_t remaining_{0};
    };

    DownloadSpaceCoordinator(
        std::filesystem::path directory,
        std::uintmax_t safety_reserve,
        SpaceQuery query = {}
    );
    ~DownloadSpaceCoordinator();

    [[nodiscard]] Lease acquire(std::uintmax_t bytes);
    void cancel() noexcept;

private:
    friend Lease;

    static void reserve(
        const std::shared_ptr<State>& state,
        std::uintmax_t bytes,
        ReservationKind kind
    );
    static void consume(
        const std::shared_ptr<State>& state,
        std::uintmax_t bytes
    );
    static void release(
        const std::shared_ptr<State>& state,
        std::uintmax_t remaining
    ) noexcept;

    std::shared_ptr<State> state_;
};

}  // namespace onedrive::sync::detail
