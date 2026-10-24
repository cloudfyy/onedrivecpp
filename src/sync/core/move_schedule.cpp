#include "sync/core/move_schedule.hpp"
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace onedrive::sync::engine_detail {

MoveSchedule build_move_schedule(
    const std::vector<LocalMoveCandidate>& moves,
    const std::unordered_map<std::string, storage::PendingMove>& pending_moves
) {
    const auto same_or_descendant = [](const std::filesystem::path& path,
                                       const std::filesystem::path& directory) {
        const auto relative = path.lexically_relative(directory);
        return relative.empty() || (!relative.native().starts_with("..") &&
                                    !relative.is_absolute());
    };

    MoveSchedule schedule{
        .dependencies = std::vector<std::vector<std::size_t>>(moves.size()),
        .actions = {},
    };
    std::vector<std::vector<std::size_t>> dependents(moves.size());
    const auto add_dependency = [&](std::size_t move,
                                    std::size_t prerequisite) {
        if (move == prerequisite ||
            std::ranges::find(schedule.dependencies[move], prerequisite) !=
                schedule.dependencies[move].end()) {
            return;
        }
        schedule.dependencies[move].push_back(prerequisite);
        dependents[prerequisite].push_back(move);
    };
    for (std::size_t index = 0; index < moves.size(); ++index) {
        for (std::size_t other = 0; other < moves.size(); ++other) {
            if (index == other) {
                continue;
            }
            if (moves[index].previous.directory &&
                same_or_descendant(
                    moves[other].previous.local_path,
                    moves[index].previous.local_path
                )) {
                add_dependency(other, index);
            }
            if (moves[other].previous.directory
                    ? same_or_descendant(
                          moves[index].destination,
                          moves[other].previous.local_path
                      )
                    : moves[index].destination ==
                          moves[other].previous.local_path) {
                add_dependency(index, other);
            }
        }
    }

    std::vector<std::size_t> remaining_dependencies(moves.size());
    for (std::size_t index = 0; index < moves.size(); ++index) {
        remaining_dependencies[index] = schedule.dependencies[index].size();
    }
    enum class MoveScheduleState {
        pending,
        scheduled,
    };
    enum class SourceState {
        occupied,
        vacated,
    };
    schedule.actions.reserve(moves.size() * 2U);
    std::vector<MoveScheduleState> scheduled(
        moves.size(), MoveScheduleState::pending
    );
    std::vector<SourceState> source_states(moves.size(), SourceState::occupied);
    const auto vacate_source = [&](std::size_t index) {
        if (source_states[index] == SourceState::vacated) {
            return;
        }
        source_states[index] = SourceState::vacated;
        for (const auto dependent : dependents[index]) {
            --remaining_dependencies[dependent];
        }
    };
    for (std::size_t index = 0; index < moves.size(); ++index) {
        const auto pending = pending_moves.find(moves[index].item.id);
        if (pending != pending_moves.end() &&
            !pending->second.staging_path.empty()) {
            schedule.actions.push_back({
                .move_index = index,
                .kind = MoveActionKind::stage,
            });
            vacate_source(index);
        }
    }

    std::size_t scheduled_count = 0;
    while (scheduled_count < moves.size()) {
        std::optional<std::size_t> ready;
        for (std::size_t index = 0; index < moves.size(); ++index) {
            if (scheduled[index] == MoveScheduleState::scheduled ||
                remaining_dependencies[index] != 0) {
                continue;
            }
            if (!ready ||
                std::ranges::count(moves[index].previous.remote_path, '/') <
                    std::ranges::count(
                        moves[*ready].previous.remote_path, '/'
                    )) {
                ready = index;
            }
        }
        if (ready) {
            scheduled[*ready] = MoveScheduleState::scheduled;
            ++scheduled_count;
            schedule.actions.push_back({
                .move_index = *ready,
                .kind = MoveActionKind::direct,
            });
            vacate_source(*ready);
            continue;
        }

        std::size_t cursor = 0;
        while (scheduled[cursor] == MoveScheduleState::scheduled) {
            ++cursor;
        }
        std::vector<std::optional<std::size_t>> seen(moves.size());
        std::size_t step = 0;
        while (!seen[cursor]) {
            seen[cursor] = step++;
            const auto prerequisite = std::ranges::find_if(
                schedule.dependencies[cursor], [&](std::size_t candidate) {
                    return scheduled[candidate] == MoveScheduleState::pending &&
                           source_states[candidate] == SourceState::occupied;
                }
            );
            if (prerequisite == schedule.dependencies[cursor].end()) {
                throw std::logic_error(
                    "move dependency graph cannot identify its cycle"
                );
            }
            cursor = *prerequisite;
        }
        schedule.actions.push_back({
            .move_index = cursor,
            .kind = MoveActionKind::stage,
        });
        vacate_source(cursor);
    }
    return schedule;
}

} // namespace onedrive::sync::engine_detail
