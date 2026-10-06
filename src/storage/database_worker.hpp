#pragma once

#include "onedrive/storage/item_database.hpp"
#include "storage/sqlite_support.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace onedrive::storage {

using item_database_detail::SqliteHandle;

struct ItemDatabase::Impl {
    Impl()
        : worker{[this] {
              run();
          }} {}

    ~Impl() {
        {
            std::lock_guard lock{mutex};
            stopping = true;
        }
        condition.notify_one();
        worker.join();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    template <typename Function>
    auto invoke(Function&& function) {
        using Result = std::invoke_result_t<Function>;

        std::promise<Result> promise;
        auto future = promise.get_future();
        {
            std::lock_guard lock{mutex};
            if (stopping) {
                throw std::runtime_error("state database is shutting down");
            }
            commands.emplace_back(
                [function = std::forward<Function>(function),
                 promise = std::move(promise)]() mutable {
                    try {
                        if constexpr (std::is_void_v<Result>) {
                            std::invoke(std::move(function));
                            promise.set_value();
                        } else {
                            promise.set_value(
                                std::invoke(std::move(function))
                            );
                        }
                    } catch (...) {
                        promise.set_exception(std::current_exception());
                    }
                }
            );
        }
        condition.notify_one();
        return future.get();
    }

    void run() {
        while (true) {
            std::move_only_function<void()> command;
            {
                std::unique_lock lock{mutex};
                condition.wait(lock, [this] {
                    return stopping || !commands.empty();
                });
                if (stopping && commands.empty()) {
                    return;
                }
                command = std::move(commands.front());
                commands.pop_front();
            }
            command();
        }
    }

    SqliteHandle database;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::move_only_function<void()>> commands;
    bool stopping{false};
    std::jthread worker;
};

}  // namespace onedrive::storage
