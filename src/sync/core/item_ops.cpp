#include "sync/core/item_ops.hpp"

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace onedrive::sync::detail {
namespace {

std::string destination_key(const std::filesystem::path& destination) {
    if (destination.empty()) {
        throw std::invalid_argument(
            "destination operation coordination requires a path"
        );
    }
    auto key = std::filesystem::absolute(destination)
                   .lexically_normal()
                   .generic_string();
    for (char& value : key) {
        if (value >= 'A' && value <= 'Z') {
            value = static_cast<char>(value - 'A' + 'a');
        }
    }
    return key;
}

}  // namespace

std::size_t ItemOperationCoordinator::ItemKeyHash::operator()(
    const ItemKey& key
) const noexcept {
    const auto scope_hash = std::hash<int>{}(
        static_cast<int>(key.scope)
    );
    const auto drive_hash = std::hash<std::string>{}(key.drive_id);
    const auto remote_hash = std::hash<std::string>{}(key.remote_id);
    const auto item_hash = drive_hash ^ (
        remote_hash + std::size_t{0x9e3779b9U} +
        (drive_hash << 6U) + (drive_hash >> 2U)
    );
    return scope_hash ^ (
        item_hash + std::size_t{0x9e3779b9U} +
        (scope_hash << 6U) + (scope_hash >> 2U)
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
    std::string remote_id,
    std::stop_token stop_token
) {
    if (drive_id.empty() || remote_id.empty()) {
        throw std::invalid_argument(
            "item operation coordination requires drive and remote IDs"
        );
    }
    ItemKey key{
        .scope = ItemKey::Scope::remote_item,
        .drive_id = std::move(drive_id),
        .remote_id = std::move(remote_id),
    };
    {
        std::unique_lock lock{mutex_};
        if (!condition_.wait(lock, stop_token, [&] {
                return !active_.contains(key);
            })) {
            throw ItemOperationCancelledError{
                "item operation coordination was cancelled"
            };
        }
        active_.insert(key);
    }
    return Lease{*this, std::move(key)};
}

ItemOperationCoordinator::Lease
ItemOperationCoordinator::acquire_destination(
    const std::filesystem::path& destination,
    std::stop_token stop_token
) {
    ItemKey key{
        .scope = ItemKey::Scope::destination,
        .drive_id = destination_key(destination),
        .remote_id = {},
    };
    {
        std::unique_lock lock{mutex_};
        if (!condition_.wait(lock, stop_token, [&] {
                return !active_.contains(key);
            })) {
            throw ItemOperationCancelledError{
                "destination operation coordination was cancelled"
            };
        }
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
