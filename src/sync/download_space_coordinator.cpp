#include "download_space_coordinator.hpp"

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
    DownloadSpaceCoordinator* owner,
    std::uintmax_t remaining
) noexcept
    : owner_{owner},
      remaining_{remaining} {}

DownloadSpaceCoordinator::Lease::~Lease() {
    release();
}

DownloadSpaceCoordinator::Lease::Lease(Lease&& other) noexcept
    : owner_{std::exchange(other.owner_, nullptr)},
      remaining_{std::exchange(other.remaining_, 0)} {}

DownloadSpaceCoordinator::Lease&
DownloadSpaceCoordinator::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
        remaining_ = std::exchange(other.remaining_, 0);
    }
    return *this;
}

void DownloadSpaceCoordinator::Lease::expand(std::uintmax_t bytes) {
    if (bytes == 0) {
        return;
    }
    if (owner_ == nullptr) {
        throw std::logic_error("cannot expand an inactive download space lease");
    }
    if (bytes > std::numeric_limits<std::uintmax_t>::max() - remaining_) {
        throw std::overflow_error("download space lease exceeds its size limit");
    }
    owner_->reserve(bytes, true);
    remaining_ += bytes;
}

void DownloadSpaceCoordinator::Lease::consume(std::uintmax_t bytes) {
    if (bytes == 0) {
        return;
    }
    if (owner_ == nullptr || bytes > remaining_) {
        throw std::logic_error(
            "download space consumption exceeds its reservation"
        );
    }
    owner_->consume(bytes);
    remaining_ -= bytes;
}

std::uintmax_t
DownloadSpaceCoordinator::Lease::remaining() const noexcept {
    return remaining_;
}

void DownloadSpaceCoordinator::Lease::release() noexcept {
    if (owner_ == nullptr) {
        return;
    }
    owner_->release(remaining_);
    owner_ = nullptr;
    remaining_ = 0;
}

DownloadSpaceCoordinator::DownloadSpaceCoordinator(
    std::filesystem::path directory,
    std::uintmax_t safety_reserve,
    SpaceQuery query
)
    : directory_{std::move(directory)},
      safety_reserve_{safety_reserve},
      query_{
          query ? std::move(query) :
                  SpaceQuery{
                      [](const std::filesystem::path& path) {
                          return std::filesystem::space(path).available;
                      }
                  }
      } {
    if (directory_.empty()) {
        throw std::invalid_argument(
            "download space coordinator requires a directory"
        );
    }
}

DownloadSpaceCoordinator::Lease DownloadSpaceCoordinator::acquire(
    std::uintmax_t bytes
) {
    if (bytes == 0) {
        const std::scoped_lock lock{mutex_};
        if (cancelled_) {
            throw DownloadSpaceCancelledError{
                "download space reservation was cancelled"
            };
        }
        ++active_leases_;
        return Lease{this, 0};
    }
    reserve(bytes, false);
    return Lease{this, bytes};
}

void DownloadSpaceCoordinator::reserve(
    std::uintmax_t bytes,
    bool expanding
) {
    std::unique_lock lock{mutex_};
    while (true) {
        if (cancelled_) {
            throw DownloadSpaceCancelledError{
                "download space reservation was cancelled"
            };
        }
        const auto available = query_(directory_);
        const bool fits =
            safety_reserve_ <= available &&
            reserved_ <= available - safety_reserve_ &&
            bytes <= available - safety_reserve_ - reserved_;
        if (fits) {
            reserved_ += bytes;
            if (!expanding) {
                ++active_leases_;
            }
            spdlog::debug(
                "{} {} download bytes; {} bytes promised with {} bytes "
                "available and {} bytes held for safety",
                expanding ? "Expanded reservation by" : "Reserved",
                bytes,
                reserved_,
                available,
                safety_reserve_
            );
            return;
        }
        const auto leases_that_can_release =
            active_leases_ - static_cast<std::size_t>(expanding);
        if (leases_that_can_release == 0) {
            throw std::runtime_error(
                std::format(
                    "download requires {} more bytes plus a {} byte "
                    "safety reserve, but only {} bytes are available",
                    bytes,
                    safety_reserve_,
                    available
                )
            );
        }
        condition_.wait(lock);
    }
}

void DownloadSpaceCoordinator::cancel() noexcept {
    {
        const std::scoped_lock lock{mutex_};
        cancelled_ = true;
    }
    condition_.notify_all();
}

void DownloadSpaceCoordinator::consume(std::uintmax_t bytes) {
    {
        const std::scoped_lock lock{mutex_};
        if (bytes > reserved_) {
            throw std::logic_error(
                "download space consumption exceeds promised bytes"
            );
        }
        reserved_ -= bytes;
    }
    condition_.notify_all();
}

void DownloadSpaceCoordinator::release(std::uintmax_t remaining) noexcept {
    {
        const std::scoped_lock lock{mutex_};
        if (remaining > reserved_ || active_leases_ == 0) {
            std::terminate();
        }
        reserved_ -= remaining;
        --active_leases_;
    }
    condition_.notify_all();
}

}  // namespace onedrive::sync::detail
