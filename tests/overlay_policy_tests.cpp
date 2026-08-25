#include "overlay_policy.h"

#include <cstdint>
#include <iostream>

namespace policy = simple_monitor::overlay_policy;

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestScreenshotTakesPriorityOverPresentation() {
    const auto observation = policy::ResolveSuppressionObservation(
        policy::TaskbarVisibility::Hidden,
        true,
        policy::PresentationVisibility::Fullscreen);
    Check(!observation.known, "screenshot should hold the committed visibility decision");

    policy::SuppressionPolicyState hidden{};
    hidden.committed = policy::SuppressionReason::FullscreenPresentation;
    const auto result = policy::ReduceSuppressionPolicy(hidden, observation, 1000);
    Check(
        result.state.committed == policy::SuppressionReason::FullscreenPresentation,
        "screenshot must not expose an overlay that was already suppressed");

    policy::SuppressionPolicyState visible{};
    const auto visible_result = policy::ReduceSuppressionPolicy(visible, observation, 1000);
    Check(
        visible_result.state.committed == policy::SuppressionReason::None,
        "screenshot must not hide an overlay that was already visible");
}

void TestObservationPriority() {
    const auto taskbar_hidden = policy::ResolveSuppressionObservation(
        policy::TaskbarVisibility::Hidden,
        false,
        policy::PresentationVisibility::Fullscreen);
    Check(taskbar_hidden.known, "hidden taskbar should be a known observation");
    Check(
        taskbar_hidden.reason == policy::SuppressionReason::TaskbarHidden,
        "taskbar suppression should take priority over presentation");

    const auto normal = policy::ResolveSuppressionObservation(
        policy::TaskbarVisibility::Visible,
        false,
        policy::PresentationVisibility::Clear);
    Check(normal.known, "normal desktop should be a known observation");
    Check(normal.reason == policy::SuppressionReason::None, "normal desktop should clear suppression");
}

void TestUnknownPreservesCommittedSuppression() {
    policy::SuppressionPolicyState state{};
    state.committed = policy::SuppressionReason::FullscreenPresentation;
    state.candidate = policy::SuppressionReason::None;
    state.candidate_since_ms = 100;
    state.candidate_active = true;

    const auto result = policy::ReduceSuppressionPolicy(state, {}, 1000);
    Check(!result.committed_changed, "unknown observation must not commit a transition");
    Check(
        result.state.committed == policy::SuppressionReason::FullscreenPresentation,
        "unknown observation must preserve committed suppression");
    Check(!result.state.candidate_active, "unknown observation should cancel stale candidate dwell");

    const auto coverage_unknown = policy::ResolveSuppressionObservation(
        policy::TaskbarVisibility::Visible,
        false,
        policy::PresentationVisibility::Unknown);
    const auto coverage_result =
        policy::ReduceSuppressionPolicy(result.state, coverage_unknown, 5000);
    Check(
        coverage_result.state.committed == policy::SuppressionReason::FullscreenPresentation,
        "unknown fullscreen coverage must not advance suppression exit");
}

void TestSuppressionHysteresis() {
    const policy::SuppressionPolicyConfig config{250, 500};
    policy::SuppressionPolicyState state{};
    const policy::SuppressionObservation enter{
        true,
        policy::SuppressionReason::FullscreenPresentation,
    };

    auto result = policy::ReduceSuppressionPolicy(state, enter, 1000, config);
    Check(!result.committed_changed, "suppression should begin as a candidate");
    state = result.state;

    result = policy::ReduceSuppressionPolicy(state, enter, 1249, config);
    Check(!result.committed_changed, "suppression should wait for enter dwell");
    state = result.state;

    result = policy::ReduceSuppressionPolicy(state, enter, 1250, config);
    Check(result.committed_changed, "suppression should commit after enter dwell");
    state = result.state;

    const policy::SuppressionObservation exit{true, policy::SuppressionReason::None};
    result = policy::ReduceSuppressionPolicy(state, exit, 2000, config);
    Check(!result.committed_changed, "restore should begin as a candidate");
    state = result.state;

    result = policy::ReduceSuppressionPolicy(state, exit, 2499, config);
    Check(!result.committed_changed, "restore should wait for the longer exit dwell");
    state = result.state;

    result = policy::ReduceSuppressionPolicy(state, exit, 2500, config);
    Check(result.committed_changed, "restore should commit after exit dwell");
}

void TestFastSuppressionProfile() {
    const policy::SuppressionPolicyConfig config{250, 500, 0, 100};
    const policy::SuppressionObservation fast_enter{
        true,
        policy::SuppressionReason::FullscreenPresentation,
        policy::SuppressionTransitionProfile::Fast,
    };

    auto result = policy::ReduceSuppressionPolicy({}, fast_enter, 1000, config);
    Check(result.committed_changed, "fast suppression should commit on the first sample");
    Check(result.candidate_dwell_ms == 0, "fast suppression should not add enter dwell");
    Check(result.required_delay_ms == 0, "fast suppression should report zero enter delay");

    const policy::SuppressionObservation fast_exit{
        true,
        policy::SuppressionReason::None,
        policy::SuppressionTransitionProfile::Fast,
    };
    result = policy::ReduceSuppressionPolicy(result.state, fast_exit, 2000, config);
    Check(!result.committed_changed, "fast restore should still reject a single clear sample");
    Check(result.required_delay_ms == 100, "fast restore should use its shorter exit delay");

    result = policy::ReduceSuppressionPolicy(result.state, fast_exit, 2099, config);
    Check(!result.committed_changed, "fast restore should wait for its full exit dwell");
    result = policy::ReduceSuppressionPolicy(result.state, fast_exit, 2100, config);
    Check(result.committed_changed, "fast restore should commit after its exit dwell");
}

void TestSuppressionProfileChangeRestartsDwell() {
    const policy::SuppressionPolicyConfig config{250, 500, 0, 100};
    policy::SuppressionPolicyState state{};
    state.committed = policy::SuppressionReason::FullscreenPresentation;
    const policy::SuppressionObservation normal_exit{
        true,
        policy::SuppressionReason::None,
    };
    const policy::SuppressionObservation fast_exit{
        true,
        policy::SuppressionReason::None,
        policy::SuppressionTransitionProfile::Fast,
    };

    auto result = policy::ReduceSuppressionPolicy(state, normal_exit, 1000, config);
    result = policy::ReduceSuppressionPolicy(result.state, fast_exit, 1400, config);
    Check(!result.committed_changed, "changing transition profile should restart candidate dwell");
    Check(result.candidate_dwell_ms == 0, "restarted profile should begin with zero dwell");

    result = policy::ReduceSuppressionPolicy(result.state, fast_exit, 1500, config);
    Check(result.committed_changed, "restarted fast profile should commit after its own dwell");
}

void TestSuppressionDoesNotDestroyOverlayIntent() {
    const auto hidden = policy::ComputeOverlayIntent(
        true,
        policy::SuppressionReason::FullscreenPresentation,
        false);
    Check(hidden.should_exist, "suppressed overlay should remain allocated");
    Check(!hidden.should_be_visible, "suppressed overlay should be hidden");

    const auto screenshot = policy::ComputeOverlayIntent(
        true,
        policy::SuppressionReason::None,
        true);
    Check(screenshot.should_exist, "screenshot should preserve overlay lifetime");
    Check(screenshot.should_be_visible, "screenshot should preserve committed visibility");
    Check(screenshot.updates_frozen, "screenshot should freeze updates");

    const auto screenshot_hidden = policy::ComputeOverlayIntent(
        true,
        policy::SuppressionReason::TaskbarHidden,
        true);
    Check(!screenshot_hidden.should_be_visible, "screenshot should preserve committed hidden state");
}

void TestSuppressionCandidateMustRemainStable() {
    const policy::SuppressionPolicyConfig config{250, 500};
    const policy::SuppressionObservation enter{
        true,
        policy::SuppressionReason::FullscreenPresentation,
    };
    const policy::SuppressionObservation clear{true, policy::SuppressionReason::None};

    auto result = policy::ReduceSuppressionPolicy({}, enter, 1000, config);
    result = policy::ReduceSuppressionPolicy(result.state, clear, 1100, config);
    Check(!result.state.candidate_active, "a clear sample should cancel an enter candidate");

    result = policy::ReduceSuppressionPolicy(result.state, enter, 1200, config);
    result = policy::ReduceSuppressionPolicy(result.state, enter, 1449, config);
    Check(!result.committed_changed, "a restarted candidate needs a full dwell interval");
    result = policy::ReduceSuppressionPolicy(result.state, enter, 1450, config);
    Check(result.committed_changed, "a stable restarted candidate should eventually commit");
}

void TestRepairSeparatesRefreshFromVisibilityCommit() {
    const auto stale = policy::ComputeOverlayRepairIntent({
        true,
        true,
        true,
        true,
        true,
        true,
    });
    Check(stale.present, "stale visible content should request a new present");
    Check(!stale.commit_visible, "stale visible content must not request another show commit");
    Check(!stale.apply_style, "stale visible content should not churn window style");
    Check(!stale.reposition, "stale visible content should not move the overlay");

    const auto hidden = policy::ComputeOverlayRepairIntent({
        true,
        false,
        true,
        true,
        true,
        false,
    });
    Check(hidden.commit_visible, "a hidden overlay should use the guarded visibility commit");
    Check(hidden.present, "a hidden overlay must present before it is shown");

    const auto missing = policy::ComputeOverlayRepairIntent({});
    Check(missing.ensure_exists, "a missing overlay should be recreated");
    Check(missing.commit_visible, "a recreated overlay should use the guarded visibility commit");

    const auto z_order = policy::ComputeOverlayRepairIntent({
        true,
        true,
        false,
        true,
        true,
        false,
    });
    Check(z_order.reposition, "lost topmost state should request z-order repair");
    Check(!z_order.present, "z-order-only repair should preserve the current surface");
    Check(!z_order.commit_visible, "z-order-only repair must not request another show commit");

    const auto layered = policy::ComputeOverlayRepairIntent({
        true,
        true,
        true,
        false,
        true,
        false,
    });
    Check(layered.apply_style, "missing layered style should be restored");
    Check(layered.present, "restored layered style should receive a new surface");
    Check(!layered.commit_visible, "style repair on a visible overlay must not call show");
}

void TestTaskbarIdentityCommitsOnce() {
    policy::TaskbarIdentityState state{};
    state.committed = {1, 10};

    auto result = policy::ReduceTaskbarIdentity(state, {1, 10}, 1000, 250);
    Check(!result.pending, "same taskbar identity should be immediately stable");
    Check(!result.committed_changed, "same taskbar identity must not recreate overlay");

    result = policy::ReduceTaskbarIdentity(result.state, {2, 20}, 1100, 250);
    Check(result.pending, "new taskbar identity should begin as a candidate");
    Check(!result.committed_changed, "new taskbar identity should not commit immediately");

    result = policy::ReduceTaskbarIdentity(result.state, {2, 20}, 1350, 250);
    Check(result.committed_changed, "stable taskbar identity should commit once");
    Check(result.state.committed == policy::TaskbarIdentity{2, 20}, "new taskbar identity should commit");

    result = policy::ReduceTaskbarIdentity(result.state, {2, 20}, 1600, 250);
    Check(!result.pending, "committed taskbar identity should be stable");
    Check(!result.committed_changed, "committed taskbar identity must be idempotent");

    result = policy::ReduceTaskbarIdentity(result.state, {}, 2000, 250);
    Check(result.pending, "missing taskbar observation should remain pending");
    Check(!result.committed_changed, "missing taskbar observation must not replace a valid identity");
    Check(result.state.committed == policy::TaskbarIdentity{2, 20}, "missing taskbar must preserve identity");

    result = policy::ReduceTaskbarIdentity(result.state, {}, 5000, 250);
    Check(!result.committed_changed, "repeated missing taskbar samples must never commit null identity");
    Check(result.state.committed == policy::TaskbarIdentity{2, 20}, "missing taskbar dwell must preserve identity");
}

}  // namespace

int main() {
    TestScreenshotTakesPriorityOverPresentation();
    TestObservationPriority();
    TestUnknownPreservesCommittedSuppression();
    TestSuppressionHysteresis();
    TestFastSuppressionProfile();
    TestSuppressionProfileChangeRestartsDwell();
    TestSuppressionDoesNotDestroyOverlayIntent();
    TestSuppressionCandidateMustRemainStable();
    TestRepairSeparatesRefreshFromVisibilityCommit();
    TestTaskbarIdentityCommitsOnce();

    if (failures != 0) {
        std::cerr << failures << " overlay policy test(s) failed\n";
        return 1;
    }
    std::cout << "overlay policy tests passed\n";
    return 0;
}
