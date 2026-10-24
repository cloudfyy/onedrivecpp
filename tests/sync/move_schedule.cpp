#include "support.hpp"
#include "move_cases.hpp"
#include "onedrive/exit_code.hpp"
#include <algorithm>

namespace onedrive::test::sync::move_cases {

namespace exit_code = onedrive::exit_code;
using namespace onedrive::test::sync;

int test_remote_move_directory() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto directory_root = temporary.path() / "directory";
    std::filesystem::create_directories(directory_root / "Old");
    {
        std::ofstream output{directory_root / "Old" / "child.txt"};
        output << "data";
    }
    FakeGraphClient directory_graph;
    directory_graph.changes = {
        {
            .id = "directory",
            .name = "New",
            .etag = "directory-etag-2",
            .parent_id = "root",
            .remote_path = "New",
            .directory = true,
        },
    };
    FakeItemStore directory_items;
    directory_items.saved_delta_link = "saved";
    directory_items.items.emplace(
        "directory", tracked_item(directory_root, "directory", "Old", true)
    );
    directory_items.items.emplace(
        "child", tracked_item(directory_root, "child", "Old/child.txt")
    );
    FakeMetrics directory_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(directory_root, false),
            directory_graph,
            directory_items,
            directory_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        std::filesystem::exists(directory_root / "Old") ||
        !std::filesystem::exists(directory_root / "New" / "child.txt") ||
        directory_graph.download_count != 0 ||
        directory_items.applied_delta.upserts.size() != 2) {
        return fail("remote directory move did not move its tracked subtree");
    }
    const auto child_state = std::ranges::find(
        directory_items.applied_delta.upserts,
        "child",
        &onedrive::storage::ItemState::remote_id
    );
    if (child_state == directory_items.applied_delta.upserts.end() ||
        child_state->remote_path != "New/child.txt" ||
        child_state->local_path != directory_root / "New" / "child.txt") {
        return fail("remote directory move did not remap descendant state");
    }
    return EXIT_SUCCESS;
}

int test_remote_move_chain() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto ordered_root = temporary.path() / "ordered-moves";
    std::filesystem::create_directories(ordered_root);
    {
        std::ofstream first{ordered_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{ordered_root / "B.txt"};
        second << "bbbb";
    }
    FakeGraphClient ordered_graph;
    ordered_graph.changes = {
        file("first", "B.txt", 4),
        file("second", "C.txt", 4),
    };
    FakeItemStore ordered_items;
    ordered_items.saved_delta_link = "saved";
    ordered_items.items.emplace(
        "first", tracked_item(ordered_root, "first", "A.txt")
    );
    ordered_items.items.emplace(
        "second", tracked_item(ordered_root, "second", "B.txt")
    );
    FakeMetrics ordered_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(ordered_root, false),
            ordered_graph,
            ordered_items,
            ordered_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        std::filesystem::exists(ordered_root / "A.txt") ||
        !std::filesystem::exists(ordered_root / "B.txt") ||
        !std::filesystem::exists(ordered_root / "C.txt") ||
        ordered_graph.download_count != 0 ||
        !ordered_items.applied_delta.blocked_upserts.empty()) {
        return fail("dependent remote move chain was not ordered safely");
    }
    {
        std::ifstream first{ordered_root / "B.txt"};
        std::ifstream second{ordered_root / "C.txt"};
        const std::string first_content{
            std::istreambuf_iterator<char>{first},
            std::istreambuf_iterator<char>{}
        };
        const std::string second_content{
            std::istreambuf_iterator<char>{second},
            std::istreambuf_iterator<char>{}
        };
        if (first_content != "aaaa" || second_content != "bbbb") {
            return fail("dependent remote move chain swapped local content");
        }
    }
    return EXIT_SUCCESS;
}

int test_remote_move_chain_recovery() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto ordered_recovery_root =
        temporary.path() / "ordered-move-recovery";
    std::filesystem::create_directories(ordered_recovery_root);
    {
        std::ofstream first{ordered_recovery_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{ordered_recovery_root / "B.txt"};
        second << "bbbb";
    }
    FakeGraphClient failed_ordered_graph;
    failed_ordered_graph.changes = {
        file("recovery-first", "B.txt", 4),
        file("recovery-second", "C.txt", 4),
    };
    FakeItemStore failed_ordered_items;
    failed_ordered_items.saved_delta_link = "saved";
    failed_ordered_items.items.emplace(
        "recovery-first",
        tracked_item(ordered_recovery_root, "recovery-first", "A.txt")
    );
    failed_ordered_items.items.emplace(
        "recovery-second",
        tracked_item(ordered_recovery_root, "recovery-second", "B.txt")
    );
    failed_ordered_items.fail_apply_delta = true;
    FakeMetrics failed_ordered_metrics;
    try {
        static_cast<void>(onedrive::sync::SyncEngine{
            config_for(ordered_recovery_root, false),
            failed_ordered_graph,
            failed_ordered_items,
            failed_ordered_metrics,
            &default_console
        }
                              .synchronize());
        return fail("dependent move commit failure did not interrupt sync");
    } catch (const std::runtime_error&) {
    }
    if (failed_ordered_items.pending_moves_by_id.size() != 2) {
        return fail("dependent move failure did not preserve both journals");
    }
    FakeGraphClient recovered_ordered_graph;
    recovered_ordered_graph.changes = failed_ordered_graph.changes;
    FakeItemStore recovered_ordered_items;
    recovered_ordered_items.saved_delta_link = "saved";
    recovered_ordered_items.items = failed_ordered_items.items;
    recovered_ordered_items.pending_moves_by_id =
        failed_ordered_items.pending_moves_by_id;
    FakeMetrics recovered_ordered_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(ordered_recovery_root, false),
            recovered_ordered_graph,
            recovered_ordered_items,
            recovered_ordered_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        recovered_ordered_graph.download_count != 0 ||
        !recovered_ordered_items.pending_moves_by_id.empty() ||
        !recovered_ordered_metrics.last_success) {
        return fail("dependent remote move journals were not recovered");
    }
    return EXIT_SUCCESS;
}

int test_remote_move_nested() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto nested_root = temporary.path() / "nested-moves";
    std::filesystem::create_directories(nested_root / "Old");
    {
        std::ofstream output{nested_root / "Old" / "before.txt"};
        output << "data";
    }
    FakeGraphClient nested_graph;
    nested_graph.changes = {
        {
            .id = "nested-directory",
            .name = "New",
            .etag = "directory-etag-2",
            .parent_id = "root",
            .remote_path = "New",
            .directory = true,
        },
        file("nested-child", "New/after.txt", 4),
    };
    FakeItemStore nested_items;
    nested_items.saved_delta_link = "saved";
    nested_items.items.emplace(
        "nested-directory",
        tracked_item(nested_root, "nested-directory", "Old", true)
    );
    nested_items.items.emplace(
        "nested-child",
        tracked_item(nested_root, "nested-child", "Old/before.txt")
    );
    FakeMetrics nested_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(nested_root, false),
            nested_graph,
            nested_items,
            nested_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        std::filesystem::exists(nested_root / "Old") ||
        std::filesystem::exists(nested_root / "New" / "before.txt") ||
        !std::filesystem::exists(nested_root / "New" / "after.txt") ||
        nested_graph.download_count != 0 ||
        !nested_items.applied_delta.blocked_upserts.empty()) {
        return fail(
            "child rename was not remapped after its parent directory move"
        );
    }
    return EXIT_SUCCESS;
}

int test_remote_move_cycle() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto cycle_root = temporary.path() / "move-cycle";
    std::filesystem::create_directories(cycle_root);
    {
        std::ofstream first{cycle_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{cycle_root / "B.txt"};
        second << "bbbb";
    }
    FakeGraphClient cycle_graph;
    cycle_graph.changes = {
        file("cycle-first", "B.txt", 4),
        file("cycle-second", "A.txt", 4),
    };
    FakeItemStore cycle_items;
    cycle_items.saved_delta_link = "saved";
    cycle_items.items.emplace(
        "cycle-first", tracked_item(cycle_root, "cycle-first", "A.txt")
    );
    cycle_items.items.emplace(
        "cycle-second", tracked_item(cycle_root, "cycle-second", "B.txt")
    );
    FakeMetrics cycle_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(cycle_root, false),
            cycle_graph,
            cycle_items,
            cycle_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        !std::filesystem::exists(cycle_root / "A.txt") ||
        !std::filesystem::exists(cycle_root / "B.txt") ||
        !cycle_items.applied_delta.blocked_upserts.empty() ||
        !cycle_items.pending_moves_by_id.empty()) {
        return fail("remote move dependency cycle was not staged safely");
    }
    {
        std::ifstream first{cycle_root / "A.txt"};
        std::ifstream second{cycle_root / "B.txt"};
        const std::string first_content{
            std::istreambuf_iterator<char>{first},
            std::istreambuf_iterator<char>{}
        };
        const std::string second_content{
            std::istreambuf_iterator<char>{second},
            std::istreambuf_iterator<char>{}
        };
        if (first_content != "bbbb" || second_content != "aaaa") {
            return fail("staged remote name exchange lost local content");
        }
    }
    if (std::ranges::any_of(
            std::filesystem::directory_iterator{cycle_root},
            [](const std::filesystem::directory_entry& entry) {
                return entry.path().filename().string().contains(
                    ".onedrive-move-"
                );
            }
        )) {
        return fail("successful name exchange retained its staging path");
    }
    return EXIT_SUCCESS;
}

int test_remote_move_three_cycle() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto three_cycle_root = temporary.path() / "three-move-cycle";
    std::filesystem::create_directories(three_cycle_root);
    {
        std::ofstream first{three_cycle_root / "A.txt"};
        first << "aaaa";
        std::ofstream second{three_cycle_root / "B.txt"};
        second << "bbbb";
        std::ofstream third{three_cycle_root / "C.txt"};
        third << "cccc";
    }
    FakeGraphClient three_cycle_graph;
    three_cycle_graph.changes = {
        file("three-first", "B.txt", 4),
        file("three-second", "C.txt", 4),
        file("three-third", "A.txt", 4),
    };
    FakeItemStore three_cycle_items;
    three_cycle_items.saved_delta_link = "saved";
    three_cycle_items.items.emplace(
        "three-first", tracked_item(three_cycle_root, "three-first", "A.txt")
    );
    three_cycle_items.items.emplace(
        "three-second", tracked_item(three_cycle_root, "three-second", "B.txt")
    );
    three_cycle_items.items.emplace(
        "three-third", tracked_item(three_cycle_root, "three-third", "C.txt")
    );
    FakeMetrics three_cycle_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(three_cycle_root, false),
            three_cycle_graph,
            three_cycle_items,
            three_cycle_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        !three_cycle_items.pending_moves_by_id.empty() ||
        !three_cycle_items.applied_delta.blocked_upserts.empty()) {
        return fail("three-item remote move cycle was not staged safely");
    }
    {
        std::ifstream first{three_cycle_root / "A.txt"};
        std::ifstream second{three_cycle_root / "B.txt"};
        std::ifstream third{three_cycle_root / "C.txt"};
        const std::string first_content{
            std::istreambuf_iterator<char>{first},
            std::istreambuf_iterator<char>{}
        };
        const std::string second_content{
            std::istreambuf_iterator<char>{second},
            std::istreambuf_iterator<char>{}
        };
        const std::string third_content{
            std::istreambuf_iterator<char>{third},
            std::istreambuf_iterator<char>{}
        };
        if (first_content != "cccc" || second_content != "aaaa" ||
            third_content != "bbbb") {
            return fail("three-item staged move cycle lost local content");
        }
    }
    return EXIT_SUCCESS;
}

int test_remote_move_directory_cycle() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto directory_cycle_root = temporary.path() / "directory-move-cycle";
    std::filesystem::create_directories(directory_cycle_root / "A");
    std::filesystem::create_directories(directory_cycle_root / "B");
    {
        std::ofstream first{directory_cycle_root / "A" / "first.txt"};
        first << "aaaa";
        std::ofstream second{directory_cycle_root / "B" / "second.txt"};
        second << "bbbb";
    }
    FakeGraphClient directory_cycle_graph;
    directory_cycle_graph.changes = {
        {
            .id = "cycle-directory-first",
            .name = "B",
            .etag = "directory-etag-2",
            .parent_id = "root",
            .remote_path = "B",
            .directory = true,
        },
        {
            .id = "cycle-directory-second",
            .name = "A",
            .etag = "directory-etag-2",
            .parent_id = "root",
            .remote_path = "A",
            .directory = true,
        },
    };
    FakeItemStore directory_cycle_items;
    directory_cycle_items.saved_delta_link = "saved";
    directory_cycle_items.items.emplace(
        "cycle-directory-first",
        tracked_item(directory_cycle_root, "cycle-directory-first", "A", true)
    );
    directory_cycle_items.items.emplace(
        "cycle-directory-second",
        tracked_item(directory_cycle_root, "cycle-directory-second", "B", true)
    );
    directory_cycle_items.items.emplace(
        "cycle-directory-first-child",
        tracked_item(
            directory_cycle_root, "cycle-directory-first-child", "A/first.txt"
        )
    );
    directory_cycle_items.items.emplace(
        "cycle-directory-second-child",
        tracked_item(
            directory_cycle_root, "cycle-directory-second-child", "B/second.txt"
        )
    );
    FakeMetrics directory_cycle_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(directory_cycle_root, false),
            directory_cycle_graph,
            directory_cycle_items,
            directory_cycle_metrics,
            &default_console
        }
                .synchronize() != exit_code::success ||
        !std::filesystem::exists(directory_cycle_root / "A" / "second.txt") ||
        !std::filesystem::exists(directory_cycle_root / "B" / "first.txt") ||
        !directory_cycle_items.pending_moves_by_id.empty() ||
        directory_cycle_items.applied_delta.upserts.size() != 4) {
        return fail("remote directory name exchange was not staged safely");
    }
    return EXIT_SUCCESS;
}

int test_remote_move_blocked_dependency() {
    const onedrive::cli::Console default_console;
    TemporaryDirectory temporary;

    const auto blocked_dependency_root =
        temporary.path() / "blocked-move-dependency";
    std::filesystem::create_directories(blocked_dependency_root / "Old");
    std::filesystem::create_directories(blocked_dependency_root / "New");
    {
        std::ofstream output{blocked_dependency_root / "Old" / "before.txt"};
        output << "data";
    }
    FakeGraphClient blocked_dependency_graph;
    blocked_dependency_graph.changes = {
        {
            .id = "blocked-directory",
            .name = "New",
            .etag = "directory-etag-2",
            .parent_id = "root",
            .remote_path = "New",
            .directory = true,
        },
        file("blocked-child", "New/after.txt", 4),
    };
    FakeItemStore blocked_dependency_items;
    blocked_dependency_items.saved_delta_link = "saved";
    blocked_dependency_items.items.emplace(
        "blocked-directory",
        tracked_item(blocked_dependency_root, "blocked-directory", "Old", true)
    );
    blocked_dependency_items.items.emplace(
        "blocked-child",
        tracked_item(blocked_dependency_root, "blocked-child", "Old/before.txt")
    );
    FakeMetrics blocked_dependency_metrics;
    if (onedrive::sync::SyncEngine{
            config_for(blocked_dependency_root, false),
            blocked_dependency_graph,
            blocked_dependency_items,
            blocked_dependency_metrics,
            &default_console
        }
                .synchronize() != exit_code::blocked ||
        !std::filesystem::exists(
            blocked_dependency_root / "Old" / "before.txt"
        ) ||
        std::filesystem::exists(
            blocked_dependency_root / "New" / "after.txt"
        ) ||
        blocked_dependency_items.applied_delta.blocked_upserts.size() != 2 ||
        std::ranges::find(
            blocked_dependency_items.applied_delta.blocked_upserts,
            "move_dependency_blocked",
            &onedrive::storage::BlockedItem::reason_code
        ) == blocked_dependency_items.applied_delta.blocked_upserts.end()) {
        return fail("failed prerequisite did not block its dependent move");
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::sync::move_cases
