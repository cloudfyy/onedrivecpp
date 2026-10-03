#include "item_operation_coordinator.hpp"

#include <functional>
#include <stdexcept>
#include <utility>

namespace onedrive::sync::detail {

std::size_t ItemOperationCoordinator::ItemKeyHash::operator()(
    const ItemKey& key
) const noexcept {
    const auto drive_hash = std::hash<std::string>{}(key.drive_id);
    const auto remote_hash = std::hash<std::string>{}(key.remote_id);
    return drive_hash ^ (
        remote_hash + std::size_t{0x9e3779b9U} +
        (drive_hash << 6U) + (drive_hash >> 2U)
    );
}

ItemOperationCoordinator::Lease::Lease(
    ItemOperationCoordinator& coordinator,
    ItemKey key
)
    : coordinator_{&coordinator},
      key_{std::move(key)} {}

ItemOperationCoordinator::Lease::~Lease() {
    release();
}

ItemOperationCoordinator::Lease::Lease(Lease&& other) noexcept
    : coordinator_{std::exchange(other.coordinator_, nullptr)},
      key_{std::move(other.key_)} {}

ItemOperationCoordinator::Lease&
ItemOperationCoordinator::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        coordinator_ = std::exchange(other.coordinator_, nullptr);
        key_ = std::move(other.key_);
    }
    return *this;
}

void ItemOperationCoordinator::Lease::release() noexcept {
    if (coordinator_ == nullptr) {
        return;
    }
    coordinator_->release(key_);
    coordinator_ = nullptr;
}

ItemOperationCoordinator::Lease ItemOperationCoordinator::acquire(
    std::string drive_id,
    std::string remote_id
) {
    if (drive_id.empty() || remote_id.empty()) {
        throw std::invalid_argument(
            "item operation coordination requires drive and remote IDs"
        );
    }
    ItemKey key{
        .drive_id = std::move(drive_id),
        .remote_id = std::move(remote_id),
    };
    {
        std::unique_lock lock{mutex_};
        condition_.wait(lock, [&] {
            return !active_.contains(key);
        });
        active_.insert(key);
    }
    return Lease{*this, std::move(key)};
}

void ItemOperationCoordinator::release(const ItemKey& key) noexcept {
    {
        std::lock_guard lock{mutex_};
        active_.erase(key);
    }
    condition_.notify_all();
}

}  // namespace onedrive::sync::detail
