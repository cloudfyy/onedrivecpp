#pragma once

#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <memory>
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

    struct State {
        std::mutex mutex;
        std::condition_variable_any condition;
        std::unordered_set<ItemKey, ItemKeyHash> active;
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

        Lease(std::shared_ptr<State> state, ItemKey key);
        void release() noexcept;

        std::shared_ptr<State> state_;
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
    static void release(
        const std::shared_ptr<State>& state,
        const ItemKey& key
    ) noexcept;

    std::shared_ptr<State> state_{std::make_shared<State>()};
};

}  // namespace onedrive::sync::detail
