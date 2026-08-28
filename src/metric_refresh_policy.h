#pragma once

#include <cstdint>

namespace simple_monitor::metric_refresh_policy {

inline bool ShouldReinitializePdhGroup(
    bool ready,
    bool has_usable_handles,
    std::uint32_t now,
    std::uint32_t last_attempt,
    std::uint32_t retry_interval) {
    if (ready && has_usable_handles) {
        return false;
    }
    if (last_attempt == 0) {
        return true;
    }

    const std::uint32_t deadline = last_attempt + retry_interval;
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

}  // namespace simple_monitor::metric_refresh_policy
