#pragma once

#include "onedrive/config/config.hpp"

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace onedrive::sync::detail {

enum class SortDirection {
    ascending,
    descending,
};

template<typename Transfer, typename SizeProjection, typename NameProjection>
void order_transfers(
    std::vector<Transfer>& transfers,
    config::TransferOrder order,
    SizeProjection size,
    NameProjection name
) {
    const auto sort_by = [&](auto projection, SortDirection direction) {
        std::stable_sort(
            transfers.begin(),
            transfers.end(),
            [&](const Transfer& left, const Transfer& right) {
                const auto& left_value = std::invoke(projection, left);
                const auto& right_value = std::invoke(projection, right);
                return direction == SortDirection::descending ?
                    left_value > right_value :
                    left_value < right_value;
            }
        );
    };

    switch (order) {
    case config::TransferOrder::default_order:
        return;
    case config::TransferOrder::size_ascending:
        sort_by(std::move(size), SortDirection::ascending);
        return;
    case config::TransferOrder::size_descending:
        sort_by(std::move(size), SortDirection::descending);
        return;
    case config::TransferOrder::name_ascending:
        sort_by(std::move(name), SortDirection::ascending);
        return;
    case config::TransferOrder::name_descending:
        sort_by(std::move(name), SortDirection::descending);
    }
}

}  // namespace onedrive::sync::detail
