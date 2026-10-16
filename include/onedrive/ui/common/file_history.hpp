#pragma once

#include "onedrive/ui/common/event.hpp"

#include <deque>
#include <stdexcept>
#include <utility>

namespace onedrive::events {

class FileHistory {
public:
    explicit FileHistory(std::size_t limit) {
        set_limit(limit);
    }

    void set_limit(std::size_t limit) {
        if (limit == 0) {
            throw std::invalid_argument{"file history limit must be positive"};
        }
        limit_ = limit;
        while (entries_.size() > limit_) {
            entries_.pop_front();
            ++omitted_;
        }
    }

    void add(FileOperationEvent event) {
        entries_.push_back(std::move(event));
        if (entries_.size() > limit_) {
            entries_.pop_front();
            ++omitted_;
        }
    }

    void clear() {
        entries_.clear();
        omitted_ = 0;
    }

    [[nodiscard]] const std::deque<FileOperationEvent>& entries() const {
        return entries_;
    }
    [[nodiscard]] std::size_t omitted() const {
        return omitted_;
    }
    [[nodiscard]] std::size_t limit() const {
        return limit_;
    }

private:
    std::size_t limit_{};
    std::size_t omitted_{};
    std::deque<FileOperationEvent> entries_;
};

} // namespace onedrive::events
