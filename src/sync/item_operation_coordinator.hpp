#pragma once

#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>

namespace onedrive::sync::detail {

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
        std::string remote_id
    );
    [[nodiscard]] Lease acquire_destination(
        const std::filesystem::path& destination
    );

private:
    void release(const ItemKey& key) noexcept;

    std::mutex mutex_;
    std::condition_variable condition_;
    std::unordered_set<ItemKey, ItemKeyHash> active_;
};

}  // namespace onedrive::sync::detail
