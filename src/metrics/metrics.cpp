#include "onedrive/metrics/metrics.hpp"

namespace onedrive::metrics {

void NullMetrics::record_sync_run(
    bool,
    std::chrono::duration<double>
) noexcept {}

}  // namespace onedrive::metrics
