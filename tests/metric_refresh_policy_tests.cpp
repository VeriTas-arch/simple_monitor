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
    using simple_monitor::metric_refresh_policy::ShouldReinitializePdhGroup;

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
