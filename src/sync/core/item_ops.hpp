#pragma once

#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <unordered_set>

namespace onedrive::sync::detail {

class ItemOperationCancelledError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ItemOperationCoordinator final {
    struct ItemKey {
        enum class Scope {
            remote_item,
            destination,
        };

        Scope scope{Scope::remote_item};
        std::string drive_id;
        std::string remote_id;

        bool operator==(const ItemKey&) const = default;
    };

    struct ItemKeyHash {
        [[nodiscard]] std::size_t operator()(const ItemKey& key) const noexcept;
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

    private:
        friend class ItemOperationCoordinator;

        Lease(ItemOperationCoordinator& coordinator, ItemKey key);
        void release() noexcept;

        ItemOperationCoordinator* coordinator_{nullptr};
        ItemKey key_;
    };

    ItemOperationCoordinator() = default;
    ~ItemOperationCoordinator() = default;

    ItemOperationCoordinator(const ItemOperationCoordinator&) = delete;
    ItemOperationCoordinator& operator=(const ItemOperationCoordinator&) =
        delete;
    ItemOperationCoordinator(ItemOperationCoordinator&&) = delete;
    ItemOperationCoordinator& operator=(ItemOperationCoordinator&&) = delete;

    [[nodiscard]] Lease acquire(
        std::string drive_id,
        std::string remote_id,
        std::stop_token stop_token = {}
    );
    [[nodiscard]] Lease acquire_destination(
        const std::filesystem::path& destination,
        std::stop_token stop_token = {}
    );

private:
    void release(const ItemKey& key) noexcept;

    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::unordered_set<ItemKey, ItemKeyHash> active_;
};

}  // namespace onedrive::sync::detail
