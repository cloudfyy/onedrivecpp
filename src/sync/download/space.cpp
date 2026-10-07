#include "sync/download/space.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>
#include <format>
#include <limits>
#include <utility>

namespace onedrive::sync::detail {

std::uintmax_t download_safety_reserve(std::uintmax_t transfer_bytes) {
    constexpr std::uintmax_t minimum_reserve =
        std::uintmax_t{256} * 1024U * 1024U;
    return std::max(minimum_reserve, transfer_bytes / 20U);
}

DownloadSpaceCoordinator::Lease::Lease(
    std::shared_ptr<State> state,
    std::uintmax_t remaining
) noexcept
    : state_{std::move(state)},
      remaining_{remaining} {}

DownloadSpaceCoordinator::Lease::~Lease() {
    release();
}

DownloadSpaceCoordinator::Lease::Lease(Lease&& other) noexcept
    : state_{std::move(other.state_)},
      remaining_{std::exchange(other.remaining_, 0)} {}

DownloadSpaceCoordinator::Lease&
DownloadSpaceCoordinator::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        remaining_ = std::exchange(other.remaining_, 0);
    }
    return *this;
}

void DownloadSpaceCoordinator::Lease::expand(std::uintmax_t bytes) {
    if (bytes == 0) {
        return;
    }
    if (!state_) {
        throw std::logic_error("cannot expand an inactive download space lease");
    }
    if (bytes > std::numeric_limits<std::uintmax_t>::max() - remaining_) {
        throw std::overflow_error("download space lease exceeds its size limit");
    }
    DownloadSpaceCoordinator::reserve(
        state_, bytes, ReservationKind::expansion
    );
    remaining_ += bytes;
}

void DownloadSpaceCoordinator::Lease::consume(std::uintmax_t bytes) {
    if (bytes == 0) {
        return;
    }
    if (!state_ || bytes > remaining_) {
        throw std::logic_error(
            "download space consumption exceeds its reservation"
        );
    }
    DownloadSpaceCoordinator::consume(state_, bytes);
    remaining_ -= bytes;
}

std::uintmax_t
DownloadSpaceCoordinator::Lease::remaining() const noexcept {
    return remaining_;
}

void DownloadSpaceCoordinator::Lease::release() noexcept {
    if (!state_) {
        return;
    }
    DownloadSpaceCoordinator::release(state_, remaining_);
    state_.reset();
    remaining_ = 0;
}

DownloadSpaceCoordinator::DownloadSpaceCoordinator(
    std::filesystem::path directory,
    std::uintmax_t safety_reserve,
    SpaceQuery query
)
    : state_{std::make_shared<State>()} {
    state_->directory = std::move(directory);
    state_->safety_reserve = safety_reserve;
    state_->query =
        query ? std::move(query) :
                SpaceQuery{
                    [](const std::filesystem::path& path) {
                        return std::filesystem::space(path).available;
                    }
                };
    if (state_->directory.empty()) {
        throw std::invalid_argument(
            "download space coordinator requires a directory"
        );
    }
}

DownloadSpaceCoordinator::~DownloadSpaceCoordinator() {
    {
        const std::scoped_lock lock{state_->mutex};
        state_->closed = true;
        state_->cancelled = true;
    }
    state_->condition.notify_all();
}

DownloadSpaceCoordinator::Lease DownloadSpaceCoordinator::acquire(
    std::uintmax_t bytes
) {
    if (bytes == 0) {
        const std::scoped_lock lock{state_->mutex};
        if (state_->cancelled) {
            throw DownloadSpaceCancelledError{
                "download space reservation was cancelled"
            };
        }
        ++state_->active_leases;
        return Lease{state_, 0};
    }
    reserve(state_, bytes, ReservationKind::initial);
    return Lease{state_, bytes};
}

void DownloadSpaceCoordinator::reserve(
    const std::shared_ptr<State>& state,
    std::uintmax_t bytes,
    ReservationKind kind
) {
    std::unique_lock lock{state->mutex};
    const bool expanding = kind == ReservationKind::expansion;
    while (true) {
        if (state->cancelled) {
            throw DownloadSpaceCancelledError{
                "download space reservation was cancelled"
            };
        }
        const auto available = state->query(state->directory);
        const bool fits =
            state->safety_reserve <= available &&
            state->reserved <= available - state->safety_reserve &&
            bytes <= available - state->safety_reserve - state->reserved;
        if (fits) {
            state->reserved += bytes;
            if (!expanding) {
                ++state->active_leases;
            }
            spdlog::debug(
                "{} {} download bytes; {} bytes promised with {} bytes "
                "available and {} bytes held for safety",
                expanding ? "Expanded reservation by" : "Reserved",
                bytes,
                state->reserved,
                available,
                state->safety_reserve
            );
            return;
        }
        const auto leases_that_can_release =
            state->active_leases - static_cast<std::size_t>(expanding);
        if (leases_that_can_release == 0) {
            throw std::runtime_error(
                std::format(
                    "download requires {} more bytes plus a {} byte "
                    "safety reserve, but only {} bytes are available",
                    bytes,
                    state->safety_reserve,
                    available
                )
            );
        }
        state->condition.wait(lock);
    }
}

void DownloadSpaceCoordinator::cancel() noexcept {
    {
        const std::scoped_lock lock{state_->mutex};
        state_->cancelled = true;
    }
    state_->condition.notify_all();
}

void DownloadSpaceCoordinator::consume(
    const std::shared_ptr<State>& state,
    std::uintmax_t bytes
) {
    {
        const std::scoped_lock lock{state->mutex};
        if (state->closed) {
            throw DownloadSpaceCancelledError{
                "download space reservation owner was destroyed"
            };
        }
        if (bytes > state->reserved) {
            throw std::logic_error(
                "download space consumption exceeds promised bytes"
            );
        }
        state->reserved -= bytes;
    }
    state->condition.notify_all();
}

void DownloadSpaceCoordinator::release(
    const std::shared_ptr<State>& state,
    std::uintmax_t remaining
) noexcept {
    {
        const std::scoped_lock lock{state->mutex};
        if (remaining > state->reserved || state->active_leases == 0) {
            std::terminate();
        }
        state->reserved -= remaining;
        --state->active_leases;
    }
    state->condition.notify_all();
}

}  // namespace onedrive::sync::detail
