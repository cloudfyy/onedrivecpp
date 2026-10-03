#include "download_space_coordinator.hpp"

#include <spdlog/spdlog.h>

#include <exception>
#include <format>
#include <utility>

namespace onedrive::sync::detail {

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
        return {};
    }
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
            ++active_leases_;
            spdlog::debug(
                "Reserved {} download bytes; {} bytes promised with {} bytes "
                "available and {} bytes held for safety",
                bytes,
                reserved_,
                available,
                safety_reserve_
            );
            return Lease{this, bytes};
        }
        if (active_leases_ == 0) {
            throw std::runtime_error(
                std::format(
                    "download requires {} additional bytes plus a {} byte "
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
