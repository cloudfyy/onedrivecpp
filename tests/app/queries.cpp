#include "support.hpp"

#include "onedrive/app/queries.hpp"

namespace {

using namespace onedrive::test::app;

int test_quota_snapshot() {
    FakeRuntimeFactory implementation;
    const onedrive::app::RuntimeFactory runtime_factory{
        onedrive::util::borrowed_proxy, implementation
    };
    const auto config = onedrive::config::Config::defaults();
    const auto snapshot =
        onedrive::app::load_quota_snapshot(config, runtime_factory);
    if (snapshot.drive.id != "drive-id" ||
        snapshot.drive.name != "Test Drive" ||
        snapshot.drive.type != "business" ||
        snapshot.drive.web_url != "https://example.test/drive" ||
        snapshot.drive.owner != "Test User" || !snapshot.drive.quota ||
        snapshot.drive.quota->total != 2'048 ||
        snapshot.drive.quota->used != 400 ||
        snapshot.drive.quota->remaining != 600 ||
        snapshot.drive.quota->deleted != 25 ||
        snapshot.drive.quota->state != "normal" ||
        implementation.graph_info_client_count != 1) {
        return fail("quota query did not return the structured drive data");
    }
    return EXIT_SUCCESS;
}

int test_status_snapshot() {
    TemporaryDirectory temporary;
    FakeRuntimeFactory implementation;
    const onedrive::app::RuntimeFactory runtime_factory{
        onedrive::util::borrowed_proxy, implementation
    };
    auto config = onedrive::config::Config::defaults();
    config.state_directory = temporary.path() / "state";
    config.sync_mode = onedrive::sync::SyncMode::upload_only;
    config.delete_policy = onedrive::sync::DeletePolicy::preserve;
    config.monitor_websocket_enabled = false;
    std::filesystem::create_directories(config.state_directory);

    const auto initial =
        onedrive::app::load_sync_status_snapshot(config, runtime_factory);
    if (initial.identity.user_display_name != "Test User" ||
        initial.identity.drive_id != "drive-id" ||
        initial.identity.drive_name != "Test Drive" ||
        initial.sync_mode != onedrive::sync::SyncMode::upload_only ||
        initial.delete_policy != onedrive::sync::DeletePolicy::preserve ||
        initial.last_run || initial.state.database_present ||
        initial.websocket_enabled ||
        implementation.graph_info_client_count != 1) {
        return fail("status query did not return the initial structured state");
    }

    const auto paths = onedrive::account::AccountState::locate(
        config.state_directory, initial.identity
    );
    std::filesystem::create_directories(paths.drive_directory);
    onedrive::metrics::FileMetrics{paths.drive_directory}.record_sync_run(
        onedrive::metrics::SyncRunOutcome::failed,
        std::chrono::milliseconds{250}
    );
    const auto failed =
        onedrive::app::load_sync_status_snapshot(config, runtime_factory);
    if (!failed.last_run ||
        failed.last_run->outcome != onedrive::metrics::SyncRunOutcome::failed ||
        failed.last_run->duration_milliseconds != 250 ||
        failed.last_run->completed_at_unix_seconds <= 0 ||
        implementation.graph_info_client_count != 2) {
        return fail("status query did not return the recorded sync result");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (test_quota_snapshot() != EXIT_SUCCESS ||
        test_status_snapshot() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
