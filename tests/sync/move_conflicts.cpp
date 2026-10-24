#include "support.hpp"
#include "move_cases.hpp"
#include "onedrive/exit_code.hpp"
#include <algorithm>

namespace onedrive::test::sync::move_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_remote_move_modified_source() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto modified_root = temporary.path() / "modified-move";
    std::filesystem::create_directories(modified_root);
    {
        std::ofstream output{modified_root / "old.txt"};
        output << "data";
    }
    FakeItemStore modified_items;
    modified_items.saved_delta_link = "saved";
    modified_items.items.emplace(
        "modified", tracked_item(modified_root, "modified", "old.txt")
    );
    {
        std::ofstream output{modified_root / "old.txt"};
        output << "user data";
    }
    FakeGraphClient modified_graph;
    modified_graph.changes = {file("modified", "new.txt", 4)};
    FakeMetrics modified_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(modified_root, false),
            modified_graph,
            modified_items,
            modified_metrics,
            &default_console
        }
                .synchronize() != exit_code::blocked ||
        !std::filesystem::exists(modified_root / "old.txt") ||
        std::filesystem::exists(modified_root / "new.txt") ||
        modified_items.applied_delta.blocked_upserts.size() != 1 ||
        modified_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_modification") {
        return fail("remote move overwrote a locally modified source");
    }
    return EXIT_SUCCESS;
}

int test_remote_move_collision() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto collision_root = temporary.path() / "collision";
    std::filesystem::create_directories(collision_root);
    {
        std::ofstream source{collision_root / "old.txt"};
        source << "data";
        std::ofstream destination{collision_root / "new.txt"};
        destination << "local";
    }
    FakeItemStore collision_items;
    collision_items.saved_delta_link = "saved";
    collision_items.items.emplace(
        "collision", tracked_item(collision_root, "collision", "old.txt")
    );
    FakeGraphClient collision_graph;
    collision_graph.changes = {file("collision", "new.txt", 4)};
    FakeMetrics collision_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(collision_root, false),
            collision_graph,
            collision_items,
            collision_metrics,
            &default_console
        }
                .synchronize() != exit_code::blocked ||
        !std::filesystem::exists(collision_root / "old.txt") ||
        collision_items.applied_delta.blocked_upserts.size() != 1 ||
        collision_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_path_conflict") {
        return fail("remote move replaced an existing local destination");
    }
    return EXIT_SUCCESS;
}

int test_remote_move_symlink() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto symlink_root = temporary.path() / "symlink-move";
    const auto outside = temporary.path() / "outside.txt";
    std::filesystem::create_directories(symlink_root);
    {
        std::ofstream output{outside};
        output << "data";
    }
    std::filesystem::create_symlink(outside, symlink_root / "old.txt");
    FakeItemStore symlink_items;
    symlink_items.saved_delta_link = "saved";
    auto symlink_state = tracked_item(symlink_root, "symlink", "old.txt");
    symlink_state.local_modified_ticks = 0;
    symlink_items.items.emplace("symlink", std::move(symlink_state));
    FakeGraphClient symlink_graph;
    symlink_graph.changes = {file("symlink", "new.txt", 4)};
    FakeMetrics symlink_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(symlink_root, false),
            symlink_graph,
            symlink_items,
            symlink_metrics,
            &default_console
        }
                .synchronize() != exit_code::blocked ||
        !std::filesystem::is_symlink(symlink_root / "old.txt") ||
        std::filesystem::exists(symlink_root / "new.txt") ||
        symlink_items.applied_delta.blocked_upserts.size() != 1 ||
        symlink_items.applied_delta.blocked_upserts[0].reason_code !=
            "local_path_conflict") {
        return fail("remote move followed or replaced a symbolic link");
    }
    return EXIT_SUCCESS;
}

int test_remote_move_cross_device() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto shared_memory_root = std::filesystem::path{"/dev/shm"};
    std::error_code device_error;
    if (std::filesystem::is_directory(shared_memory_root, device_error) &&
        !device_error) {
        const auto cross_source_root = temporary.path() / "cross-device-source";
        std::filesystem::create_directories(cross_source_root);
        const auto cross_source = cross_source_root / "old.txt";
        {
            std::ofstream output{cross_source};
            output << "data";
        }
        const auto cross_destination_root =
            shared_memory_root /
            ("onedrive-cpp-" + temporary.path().filename().string());
        struct DestinationCleanup {
            std::filesystem::path path;
            ~DestinationCleanup() {
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        } cleanup{cross_destination_root};
        std::filesystem::create_directory(cross_destination_root);
        const auto cross_destination = cross_destination_root / "new.txt";
        auto cross_change = file(
            "cross-device",
            cross_destination.lexically_relative("/").generic_string(),
            4
        );
        FakeGraphClient cross_graph;
        cross_graph.changes = {cross_change};
        FakeItemStore cross_items;
        cross_items.saved_delta_link = "saved";
        auto cross_state =
            tracked_item(cross_source_root, "cross-device", "old.txt");
        cross_state.remote_path =
            cross_source.lexically_relative("/").generic_string();
        cross_items.items.emplace("cross-device", std::move(cross_state));
        FakeMetrics cross_metrics;
        auto cross_config = config_for("/", false);
        cross_config.nosync_enabled = false;
        cross_config.sync_permissions =
            onedrive::config::SyncPermissionsMode::umask;
        if (onedrive::sync::SyncEngine{
                cross_config,
                cross_graph,
                cross_items,
                cross_metrics,
                &default_console
            }
                    .synchronize() != exit_code::blocked ||
            !std::filesystem::exists(cross_source) ||
            std::filesystem::exists(cross_destination) ||
            cross_items.applied_delta.blocked_upserts.size() != 1 ||
            cross_items.applied_delta.blocked_upserts[0].reason_code !=
                "cross_device_move" ||
            !cross_items.pending_moves_by_id.empty() ||
            !cross_metrics.last_success) {
            return fail("cross-device remote move was not blocked safely");
        }
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::move_cases
