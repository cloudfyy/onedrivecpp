#pragma once

#include "onedrive/config/config.hpp"

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace onedrive::sync::detail {

template<typename Transfer, typename SizeProjection, typename NameProjection>
void order_transfers(
    std::vector<Transfer>& transfers,
    config::TransferOrder order,
    SizeProjection size,
    NameProjection name
) {
    const auto sort_by = [&](auto projection, bool descending) {
        std::stable_sort(
            transfers.begin(),
            transfers.end(),
            [&](const Transfer& left, const Transfer& right) {
                const auto& left_value = std::invoke(projection, left);
                const auto& right_value = std::invoke(projection, right);
                return descending ?
                    left_value > right_value :
                    left_value < right_value;
            }
        );
    };

    switch (order) {
    case config::TransferOrder::default_order:
        return;
    case config::TransferOrder::size_ascending:
        sort_by(std::move(size), false);
        return;
    case config::TransferOrder::size_descending:
        sort_by(std::move(size), true);
        return;
    case config::TransferOrder::name_ascending:
        sort_by(std::move(name), false);
        return;
    case config::TransferOrder::name_descending:
        sort_by(std::move(name), true);
    }
}

}  // namespace onedrive::sync::detail
