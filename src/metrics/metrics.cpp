#include "onedrive/metrics/metrics.hpp"

namespace onedrive::metrics {

void NullMetrics::record_sync_run(
    SyncRunOutcome,
    std::chrono::duration<double>
) noexcept {}

}  // namespace onedrive::metrics
