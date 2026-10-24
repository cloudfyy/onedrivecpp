#pragma once

#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace onedrive::sync::engine_detail {

struct LocalMoveCandidate {
    graph::RemoteItem item;
    storage::ItemState previous;
    std::filesystem::path destination;
};

enum class MoveActionKind {
    direct,
    stage,
};

struct MoveAction {
    std::size_t move_index{0};
    MoveActionKind kind{MoveActionKind::direct};
};

struct MoveSchedule {
    std::vector<std::vector<std::size_t>> dependencies;
    std::vector<MoveAction> actions;
};

MoveSchedule build_move_schedule(
    const std::vector<LocalMoveCandidate>& moves,
    const std::unordered_map<std::string, storage::PendingMove>& pending_moves
);

} // namespace onedrive::sync::engine_detail
