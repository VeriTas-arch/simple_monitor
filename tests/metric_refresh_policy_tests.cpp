#include "metric_refresh_policy.h"

#include <cstdint>
#include <iostream>
#include <limits>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) {
        return;
    }

    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

}  // namespace

int main() {
    using simple_monitor::metric_refresh_policy::AggregateBusiestGpuEngine;
    using simple_monitor::metric_refresh_policy::CanonicalGpuEngineKey;
    using simple_monitor::metric_refresh_policy::ClassifyPdhSample;
    using simple_monitor::metric_refresh_policy::GpuEngineSampleView;
    using simple_monitor::metric_refresh_policy::PdhSampleDisposition;
    using simple_monitor::metric_refresh_policy::ShouldReinitializePdhGroup;

    Expect(
        ClassifyPdhSample(true, false) == PdhSampleDisposition::UseValue,
        "a valid PDH sample should be published");
    Expect(
        ClassifyPdhSample(false, true) == PdhSampleDisposition::KeepPrevious,
        "a transient PDH calculation status should preserve the query and value");
    Expect(
        ClassifyPdhSample(false, false) == PdhSampleDisposition::Reinitialize,
        "a non-transient PDH failure should rebuild the query");

    const std::wstring_view engine_a_pid_1 =
        L"pid_100_luid_0x00000000_0x00001234_phys_0_eng_0_engtype_3D";
    const std::wstring_view engine_a_pid_2 =
        L"pid_200_luid_0x00000000_0x00001234_phys_0_eng_0_engtype_3D";
    Expect(
        CanonicalGpuEngineKey(engine_a_pid_1) ==
            CanonicalGpuEngineKey(engine_a_pid_2),
        "GPU samples from different processes should share one physical engine key");
    Expect(
        CanonicalGpuEngineKey(L"pid_100_engtype_Copy_0") == L"_engtype_Copy_0",
        "GPU engine keys should tolerate instance names without a LUID");

    const std::vector<GpuEngineSampleView> gpu_samples{
        {engine_a_pid_1, 20.0},
        {engine_a_pid_2, 30.0},
        {L"pid_300_luid_0x00000000_0x00001234_phys_0_eng_1_engtype_Copy", 60.0},
    };
    Expect(
        AggregateBusiestGpuEngine(gpu_samples) == 60.0,
        "overall GPU usage should use the busiest aggregated engine");
    Expect(
        AggregateBusiestGpuEngine({
            {engine_a_pid_1, 70.0},
            {engine_a_pid_2, 50.0},
        }) == 100.0,
        "busiest GPU engine utilization should remain percentage bounded");

    Expect(
        !ShouldReinitializePdhGroup(true, true, 60000, 1000, 5000),
        "a healthy query must remain persistent after the retry interval");
    Expect(
        ShouldReinitializePdhGroup(false, false, 1000, 0, 5000),
        "a group with no prior attempt should initialize immediately");
    Expect(
        ShouldReinitializePdhGroup(true, false, 6000, 1000, 5000),
        "a nominally ready group with missing handles should recover");
    Expect(
        !ShouldReinitializePdhGroup(false, true, 5999, 1000, 5000),
        "an unavailable group must honor the retry backoff");
    Expect(
        ShouldReinitializePdhGroup(false, true, 6000, 1000, 5000),
        "an unavailable group should retry when the backoff expires");

    const std::uint32_t near_wrap = std::numeric_limits<std::uint32_t>::max() - 100;
    Expect(
        ShouldReinitializePdhGroup(false, true, 100, near_wrap, 200),
        "retry deadlines must remain valid across the 32-bit tick wrap");

    if (failures != 0) {
        std::cerr << failures << " metric refresh policy test(s) failed\n";
        return 1;
    }

    std::cout << "metric refresh policy tests passed\n";
    return 0;
}
