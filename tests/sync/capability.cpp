#include "onedrive/sync/capabilities.hpp"

#include <array>
#include <cstdlib>
#include <type_traits>

namespace {

using onedrive::sync::capabilities_for;
using onedrive::sync::DeletePolicy;
using onedrive::sync::ExecutionMode;
using onedrive::sync::SyncCapabilities;
using onedrive::sync::SyncMode;

constexpr auto bidirectional = capabilities_for(
    SyncMode::bidirectional, DeletePolicy::propagate, ExecutionMode::apply
);
static_assert(bidirectional.downloads());
static_assert(bidirectional.removes_local_items());
static_assert(bidirectional.uploads());
static_assert(bidirectional.removes_remote_items());
static_assert(bidirectional.resolves_conflicts());
static_assert(bidirectional.writes_journal());
static_assert(bidirectional.monitors());

constexpr auto preview = capabilities_for(
    SyncMode::bidirectional, DeletePolicy::propagate, ExecutionMode::preview
);
static_assert(preview.previews());
static_assert(preview.plans_downloads());
static_assert(preview.plans_local_deletions());
static_assert(preview.plans_uploads());
static_assert(preview.plans_remote_deletions());
static_assert(!preview.downloads());
static_assert(!preview.removes_local_items());
static_assert(!preview.uploads());
static_assert(!preview.removes_remote_items());
static_assert(!preview.resolves_conflicts());
static_assert(!preview.writes_journal());
static_assert(preview.monitors());

constexpr auto upload_only = capabilities_for(
    SyncMode::upload_only, DeletePolicy::preserve, ExecutionMode::apply
);
static_assert(!upload_only.plans_downloads());
static_assert(!upload_only.downloads());
static_assert(!upload_only.plans_local_deletions());
static_assert(!upload_only.removes_local_items());
static_assert(upload_only.plans_uploads());
static_assert(upload_only.uploads());
static_assert(!upload_only.plans_remote_deletions());
static_assert(!upload_only.removes_remote_items());
static_assert(upload_only.resolves_conflicts());
static_assert(upload_only.writes_journal());
static_assert(upload_only.monitors());

constexpr auto download_only = capabilities_for(
    SyncMode::download_only, DeletePolicy::propagate, ExecutionMode::apply
);
static_assert(download_only.plans_downloads());
static_assert(download_only.downloads());
static_assert(download_only.plans_local_deletions());
static_assert(download_only.removes_local_items());
static_assert(!download_only.plans_uploads());
static_assert(!download_only.uploads());
static_assert(!download_only.plans_remote_deletions());
static_assert(!download_only.removes_remote_items());
static_assert(download_only.resolves_conflicts());
static_assert(download_only.writes_journal());
static_assert(download_only.monitors());

constexpr auto preserving_download = capabilities_for(
    SyncMode::download_only, DeletePolicy::preserve, ExecutionMode::apply
);
static_assert(preserving_download.downloads());
static_assert(!preserving_download.plans_local_deletions());
static_assert(!preserving_download.removes_local_items());

static_assert(!std::is_default_constructible_v<SyncCapabilities>);
static_assert(std::is_trivially_copyable_v<SyncCapabilities>);

int test_capability_matrix() {
    constexpr std::array sync_modes{
        SyncMode::bidirectional,
        SyncMode::upload_only,
        SyncMode::download_only,
    };
    constexpr std::array delete_policies{
        DeletePolicy::propagate,
        DeletePolicy::preserve,
    };
    constexpr std::array execution_modes{
        ExecutionMode::apply,
        ExecutionMode::preview,
    };

    for (const auto sync_mode : sync_modes) {
        for (const auto delete_policy : delete_policies) {
            for (const auto execution_mode : execution_modes) {
                const auto capabilities =
                    capabilities_for(sync_mode, delete_policy, execution_mode);
                const bool previews = execution_mode == ExecutionMode::preview;
                const bool plans_downloads = sync_mode != SyncMode::upload_only;
                const bool plans_uploads = sync_mode != SyncMode::download_only;
                const bool propagates_deletions =
                    delete_policy == DeletePolicy::propagate;

                if (capabilities.sync_mode() != sync_mode ||
                    capabilities.delete_policy() != delete_policy ||
                    capabilities.execution_mode() != execution_mode ||
                    capabilities.previews() != previews ||
                    capabilities.plans_downloads() != plans_downloads ||
                    capabilities.downloads() !=
                        (!previews && plans_downloads) ||
                    capabilities.plans_local_deletions() !=
                        (plans_downloads && propagates_deletions) ||
                    capabilities.removes_local_items() !=
                        (!previews && plans_downloads && propagates_deletions
                        ) ||
                    capabilities.plans_uploads() != plans_uploads ||
                    capabilities.uploads() != (!previews && plans_uploads) ||
                    capabilities.plans_remote_deletions() !=
                        (plans_uploads && propagates_deletions) ||
                    capabilities.removes_remote_items() !=
                        (!previews && plans_uploads && propagates_deletions) ||
                    capabilities.resolves_conflicts() != !previews ||
                    capabilities.writes_journal() != !previews ||
                    !capabilities.monitors()) {
                    return EXIT_FAILURE;
                }
            }
        }
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_capability_matrix();
}
