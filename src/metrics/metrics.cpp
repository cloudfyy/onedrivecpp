#include "onedrive/metrics/metrics.hpp"

#include "util/atomic_file.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <utility>

namespace onedrive::metrics {
namespace {

constexpr mode_t private_file_mode = S_IRUSR | S_IWUSR;

std::filesystem::path status_path(
    const std::filesystem::path& state_directory
) {
    return state_directory / "sync-status.json";
}

std::string_view outcome_name(SyncRunOutcome outcome) {
    return outcome == SyncRunOutcome::succeeded ? "succeeded" : "failed";
}

}  // namespace

void NullMetrics::record_sync_run(
    SyncRunOutcome,
    std::chrono::duration<double>
) noexcept {}

FileMetrics::FileMetrics(std::filesystem::path state_directory)
    : state_directory_{std::move(state_directory)} {}

void FileMetrics::record_sync_run(
    SyncRunOutcome outcome,
    std::chrono::duration<double> duration
) noexcept {
    try {
        const auto completed_at = std::chrono::duration_cast<
            std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        );
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(duration);
        onedrive::util::write_file_atomically(
            status_path(state_directory_),
            nlohmann::json{
                {"outcome", outcome_name(outcome)},
                {"completed_at_unix_seconds", completed_at.count()},
                {
                    "duration_milliseconds",
                    std::max<std::int64_t>(0, elapsed.count())
                },
            }.dump(2) + "\n",
            private_file_mode,
            "synchronization status file"
        );
    } catch (const std::exception& error) {
        spdlog::error(
            "Cannot persist synchronization status: {}",
            error.what()
        );
    }
}

std::optional<SyncRunStatus>
load_sync_run_status(const std::filesystem::path& state_directory) {
    std::ifstream input{status_path(state_directory)};
    if (!input) {
        if (!std::filesystem::exists(status_path(state_directory))) {
            return std::nullopt;
        }
        throw std::runtime_error(
            "cannot open synchronization status file: " +
            status_path(state_directory).string()
        );
    }
    try {
        const auto document = nlohmann::json::parse(input);
        const auto outcome = document.at("outcome").get<std::string>();
        const auto completed =
            document.at("completed_at_unix_seconds").get<std::int64_t>();
        const auto duration =
            document.at("duration_milliseconds").get<std::uint64_t>();
        if ((outcome != "succeeded" && outcome != "failed") ||
            completed < 0) {
            throw std::runtime_error(
                "synchronization status file contains invalid values"
            );
        }
        return SyncRunStatus{
            .outcome = outcome == "succeeded" ?
                SyncRunOutcome::succeeded :
                SyncRunOutcome::failed,
            .completed_at_unix_seconds = completed,
            .duration_milliseconds = duration,
        };
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(
            "cannot parse synchronization status file: " +
            std::string{error.what()}
        );
    }
}

}  // namespace onedrive::metrics
