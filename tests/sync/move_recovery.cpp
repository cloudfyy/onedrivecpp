#include "support.hpp"

namespace {

using namespace onedrive::test::sync;

int journal_boundary_failures() {
    const onedrive::cli::Console console;
    for (const bool cleanup_failure : {false, true}) {
        TemporaryDirectory temporary;
        const auto source = temporary.path() / "old.txt";
        const auto destination = temporary.path() / "new.txt";
        onedrive::test::write_file(source, "data");
        FakeGraphClient graph;
        graph.changes = {file("move", "new.txt", 4)};
        FakeItemStore items;
        items.saved_delta_link = "saved";
        items.items.emplace(
            "move", tracked_item(temporary.path(), "move", "old.txt")
        );
        FakeMetrics metrics;
        items.before_pending_move_save = [&](const auto& pending) {
            if (pending.source_path != source ||
                pending.destination_path != destination ||
                pending.source_inode == 0) {
                throw std::runtime_error{"incorrect move journal"};
            }
            onedrive::test::write_file(destination, "intruder");
        };
        items.fail_pending_move_remove = cleanup_failure;
        onedrive::sync::SyncEngine engine{
            config_for(temporary.path(), false), graph, items, metrics, &console
        };
        if (cleanup_failure) {
            if (!onedrive::test::throws_with(
                    [&] { static_cast<void>(engine.synchronize()); },
                    "simulated move journal cleanup failure"
                ) ||
                items.pending_moves_by_id.size() != 1 ||
                !items.removed_pending_moves.empty()) {
                return fail(
                    "failed journal cleanup did not retain recoverable move "
                    "state"
                );
            }
        } else if (engine.synchronize() != 2 ||
                   items.applied_delta.blocked_upserts.size() != 1 ||
                   items.applied_delta.blocked_upserts[0].reason_code !=
                       "local_path_conflict" ||
                   !items.pending_moves_by_id.empty() ||
                   items.removed_pending_moves !=
                       std::vector<std::string>{"move"}) {
            return fail(
                "move conflict after journaling did not discard its "
                "uninstalled journal"
            );
        }
        if (onedrive::test::read_file(source) != "data" ||
            onedrive::test::read_file(destination) != "intruder" ||
            graph.download_count != 0 ||
            items.items.at("move").local_path != source) {
            return fail(
                "journal-boundary conflict overwrote a local object or "
                "advanced state"
            );
        }
        items.before_pending_move_save = {};
        items.fail_pending_move_remove = false;
        std::filesystem::remove(destination);
        if (engine.synchronize() != 0 || std::filesystem::exists(source) ||
            onedrive::test::read_file(destination) != "data" ||
            !items.pending_moves_by_id.empty() || graph.download_count != 0 ||
            items.items.at("move").local_path != destination ||
            !metrics.last_success) {
            return fail(
                "journal-boundary failure could not recover without "
                "redownloading"
            );
        }
    }
    return EXIT_SUCCESS;
}

int invalid_recovery_journals() {
    const onedrive::cli::Console console;
    enum class Mismatch { source, destination, type, identity };
    for (const auto mismatch :
         {Mismatch::source,
          Mismatch::destination,
          Mismatch::type,
          Mismatch::identity}) {
        TemporaryDirectory temporary;
        const auto source = temporary.path() / "old.txt";
        const auto destination = temporary.path() / "new.txt";
        onedrive::test::write_file(source, "data");
        FakeGraphClient graph;
        graph.changes = {file("move", "new.txt", 4)};
        FakeItemStore items;
        items.saved_delta_link = "saved";
        const auto tracked = tracked_item(temporary.path(), "move", "old.txt");
        items.items.emplace("move", tracked);
        struct stat status{};
        if (::stat(source.c_str(), &status) != 0) {
            return fail("cannot inspect recovery source identity");
        }
        onedrive::storage::PendingMove pending{
            .drive_id = "me",
            .remote_id = "move",
            .source_path = source,
            .destination_path = destination,
            .source_device = static_cast<std::uint64_t>(status.st_dev),
            .source_inode = static_cast<std::uint64_t>(status.st_ino),
        };
        const auto valid = pending;
        if (mismatch == Mismatch::source) {
            pending.source_path = temporary.path() / "wrong-source";
        } else if (mismatch == Mismatch::destination) {
            pending.destination_path = temporary.path() / "wrong-destination";
        } else if (mismatch == Mismatch::type) {
            pending.directory = true;
        } else {
            pending.source_inode = 0;
        }
        items.pending_moves_by_id.emplace("move", pending);
        FakeMetrics metrics;
        onedrive::sync::SyncEngine engine{
            config_for(temporary.path(), false), graph, items, metrics, &console
        };
        if (engine.synchronize() != 2 ||
            items.applied_delta.blocked_upserts.size() != 1 ||
            items.applied_delta.blocked_upserts[0].reason_code !=
                "pending_move_conflict" ||
            items.pending_moves_by_id.size() != 1 ||
            !items.removed_pending_moves.empty() ||
            onedrive::test::read_file(source) != "data" ||
            std::filesystem::exists(destination) ||
            items.items.at("move").local_path != source ||
            graph.download_count != 0) {
            return fail(
                "inconsistent durable move journal was adopted or discarded"
            );
        }
        items.pending_moves_by_id.insert_or_assign("move", valid);
        if (engine.synchronize() != 0 || std::filesystem::exists(source) ||
            onedrive::test::read_file(destination) != "data" ||
            !items.pending_moves_by_id.empty() ||
            items.items.at("move").local_path != destination ||
            graph.download_count != 0) {
            return fail("corrected durable move journal did not recover");
        }
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (const int result = journal_boundary_failures();
        result != EXIT_SUCCESS) {
        return result;
    }
    return invalid_recovery_journals();
}
