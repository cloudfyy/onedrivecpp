#include "onedrive/metrics/metrics.hpp"
#include "support/common.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

int test_file_metrics() {
    onedrive::test::TemporaryDirectory temporary;
    onedrive::metrics::FileMetrics metrics{temporary.path()};
    metrics.record_sync_run(
        onedrive::metrics::SyncRunOutcome::succeeded,
        std::chrono::milliseconds{1250}
    );
    const auto status =
        onedrive::metrics::load_sync_run_status(temporary.path());
    if (!status ||
        status->outcome != onedrive::metrics::SyncRunOutcome::succeeded ||
        status->completed_at_unix_seconds <= 0 ||
        status->duration_milliseconds != 1250) {
        return onedrive::test::fail(
            "file metrics did not persist synchronization status"
        );
    }
    metrics.record_sync_run(
        onedrive::metrics::SyncRunOutcome::failed, std::chrono::milliseconds{-1}
    );
    const auto failed =
        onedrive::metrics::load_sync_run_status(temporary.path());
    if (!failed ||
        failed->outcome != onedrive::metrics::SyncRunOutcome::failed ||
        failed->duration_milliseconds != 0) {
        return onedrive::test::fail(
            "file metrics did not persist a failed synchronization status"
        );
    }

    const auto missing = temporary.path() / "missing";
    if (onedrive::metrics::load_sync_run_status(missing)) {
        return onedrive::test::fail(
            "missing synchronization status was not optional"
        );
    }
    {
        std::ofstream output{temporary.path() / "sync-status.json"};
        output << R"({"outcome":"unknown",)"
                  R"("completed_at_unix_seconds":1,)"
                  R"("duration_milliseconds":2})";
    }
    try {
        static_cast<void>(
            onedrive::metrics::load_sync_run_status(temporary.path())
        );
        return onedrive::test::fail(
            "invalid synchronization status was accepted"
        );
    } catch (const std::runtime_error&) {
    }
    {
        std::ofstream output{temporary.path() / "sync-status.json"};
        output << "{";
    }
    try {
        static_cast<void>(
            onedrive::metrics::load_sync_run_status(temporary.path())
        );
        return onedrive::test::fail(
            "malformed synchronization status was accepted"
        );
    } catch (const std::runtime_error&) {
    }
    onedrive::metrics::FileMetrics{temporary.path() / "missing" / "drive"}
        .record_sync_run(
            onedrive::metrics::SyncRunOutcome::succeeded,
            std::chrono::milliseconds{1}
        );
    if (std::filesystem::exists(
            temporary.path() / "missing" / "drive" / "sync-status.json"
        )) {
        return onedrive::test::fail(
            "file metrics reported success after a persistence failure"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_file_metrics();
}
