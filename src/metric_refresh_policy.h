#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace simple_monitor::metric_refresh_policy {

enum class PdhSampleDisposition {
    UseValue,
    KeepPrevious,
    Reinitialize,
};

constexpr PdhSampleDisposition ClassifyPdhSample(
    bool value_valid,
    bool transient_calculation_status) {
    if (value_valid) {
        return PdhSampleDisposition::UseValue;
    }
    return transient_calculation_status
        ? PdhSampleDisposition::KeepPrevious
        : PdhSampleDisposition::Reinitialize;
}

struct GpuEngineSampleView {
    std::wstring_view instance_name;
    double utilization = 0.0;
};

inline std::wstring_view CanonicalGpuEngineKey(std::wstring_view instance_name) {
    std::size_t key_start = std::wstring_view::npos;
    constexpr std::wstring_view markers[] = {
        L"_luid_",
        L"_phys_",
        L"_eng_",
        L"_engtype_",
    };
    for (std::wstring_view marker : markers) {
        const std::size_t position = instance_name.find(marker);
        if (position != std::wstring_view::npos &&
            (key_start == std::wstring_view::npos || position < key_start)) {
            key_start = position;
        }
    }
    return key_start == std::wstring_view::npos
        ? instance_name
        : instance_name.substr(key_start);
}

inline double AggregateBusiestGpuEngine(
    const std::vector<GpuEngineSampleView>& samples) {
    std::unordered_map<std::wstring_view, double> engine_totals;
    double busiest = 0.0;
    for (const GpuEngineSampleView& sample : samples) {
        const double utilization = std::max(0.0, sample.utilization);
        const std::wstring_view key = CanonicalGpuEngineKey(sample.instance_name);
        if (key.empty()) {
            busiest = std::max(busiest, utilization);
            continue;
        }
        double& total = engine_totals[key];
        total += utilization;
        busiest = std::max(busiest, total);
    }
    return std::min(100.0, busiest);
}

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
