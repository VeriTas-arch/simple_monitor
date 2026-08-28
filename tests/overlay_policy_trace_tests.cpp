#include "overlay_policy.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>

namespace policy = simple_monitor::overlay_policy;

namespace {

int failures = 0;

void CheckStep(bool condition, const char* trace, std::size_t step, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << trace << " step " << step << ": " << message << '\n';
        ++failures;
    }
}

struct SuppressionTraceStep {
    std::uint64_t now_ms;
    policy::TaskbarVisibility taskbar;
    policy::PresentationVisibility presentation;
    bool screenshot_foreground;
    policy::SuppressionTransitionProfile transition_profile;
    policy::SuppressionReason expected_committed;
    bool expected_visible;
    bool expected_frozen;
    bool expected_commit_changed;
};

constexpr policy::SuppressionPolicyConfig kSuppressionConfig{
    250,
    500,
    0,
    100,
};

template <std::size_t Size>
void ReplaySuppressionTrace(
    const char* name,
    const SuppressionTraceStep (&steps)[Size],
    policy::SuppressionReason initial = policy::SuppressionReason::None) {
    policy::SuppressionPolicyState state{};
    state.committed = initial;

    for (std::size_t index = 0; index < Size; ++index) {
        const SuppressionTraceStep& step = steps[index];
        auto observation = policy::ResolveSuppressionObservation(
            step.taskbar,
            step.screenshot_foreground,
            step.presentation);
        if (observation.known && step.taskbar == policy::TaskbarVisibility::Visible) {
            observation.transition_profile = step.transition_profile;
        }

        const auto result = policy::ReduceSuppressionPolicy(
            state,
            observation,
            step.now_ms,
            kSuppressionConfig);
        state = result.state;
        const auto intent = policy::ComputeOverlayIntent(
            true,
            state.committed,
            step.screenshot_foreground);

        CheckStep(
            intent.should_exist,
            name,
            index,
            "suppression discarded the overlay lifetime");
        CheckStep(
            state.committed == step.expected_committed,
            name,
            index,
            "unexpected committed suppression");
        CheckStep(
            intent.should_be_visible == step.expected_visible,
            name,
            index,
            "unexpected visibility intent");
        CheckStep(
            intent.updates_frozen == step.expected_frozen,
            name,
            index,
            "unexpected update-freeze intent");
        CheckStep(
            result.committed_changed == step.expected_commit_changed,
            name,
            index,
            "unexpected suppression commit transition");
    }
}

constexpr SuppressionTraceStep kPowerPointSlideshowToEditor[] = {
    {0, policy::TaskbarVisibility::Visible, policy::PresentationVisibility::Clear, false,
     policy::SuppressionTransitionProfile::Default, policy::SuppressionReason::None, true, false, false},
    {1000, policy::TaskbarVisibility::Visible, policy::PresentationVisibility::Fullscreen, false,
     policy::SuppressionTransitionProfile::Fast, policy::SuppressionReason::FullscreenPresentation, false, true, true},
    {1100, policy::TaskbarVisibility::Visible, policy::PresentationVisibility::Fullscreen, false,
     policy::SuppressionTransitionProfile::Fast, policy::SuppressionReason::FullscreenPresentation, false, true, false},
    {1200, policy::TaskbarVisibility::Visible, policy::PresentationVisibility::Clear, false,
     policy::SuppressionTransitionProfile::Fast, policy::SuppressionReason::FullscreenPresentation, false, true, false},
    {1299, policy::TaskbarVisibility::Visible, policy::PresentationVisibility::Clear, false,
     policy::SuppressionTransitionProfile::Fast, policy::SuppressionReason::FullscreenPresentation, false, true, false},
    {1300, policy::TaskbarVisibility::Visible, policy::PresentationVisibility::Clear, false,
     policy::SuppressionTransitionProfile::Fast, policy::SuppressionReason::None, true, false, true},
};

constexpr SuppressionTraceStep kStartSearchTransient[] = {
    {0, policy::TaskbarVisibility::Visible, policy::PresentationVisibility::Clear, false,
     policy::SuppressionTransitionProfile::Default, policy::SuppressionReason::None, true, false, false},
    {100, policy::TaskbarVisibility::Visible, policy::PresentationVisibility::Unknown, false,
     policy::SuppressionTransitionProfile::Default, policy::SuppressionReason::None, true, false, false},
    {200, policy::TaskbarVisibility::Unknown, policy::PresentationVisibility::Clear, false,
     policy::SuppressionTransitionProfile::Default, policy::SuppressionReason::None, true, false, false},
    {300, policy::TaskbarVisibility::Visible, policy::PresentationVisibility::Clear, false,
     policy::SuppressionTransitionProfile::Default, policy::SuppressionReason::None, true, false, false},
};

constexpr SuppressionTraceStep kScreenshotFreeze[] = {
    {0, policy::TaskbarVisibility::Visible, policy::PresentationVisibility::Clear, false,
     policy::SuppressionTransitionProfile::Default, policy::SuppressionReason::None, true, false, false},
    {100, policy::TaskbarVisibility::Hidden, policy::PresentationVisibility::Fullscreen, true,
     policy::SuppressionTransitionProfile::Default, policy::SuppressionReason::None, true, true, false},
    {200, policy::TaskbarVisibility::Visible, policy::PresentationVisibility::Fullscreen, true,
     policy::SuppressionTransitionProfile::Fast, policy::SuppressionReason::None, true, true, false},
    {300, policy::TaskbarVisibility::Visible, policy::PresentationVisibility::Clear, false,
     policy::SuppressionTransitionProfile::Default, policy::SuppressionReason::None, true, false, false},
};

constexpr SuppressionTraceStep kTaskbarAutoHide[] = {
    {0, policy::TaskbarVisibility::Visible, policy::PresentationVisibility::Clear, false,
     policy::SuppressionTransitionProfile::Default, policy::SuppressionReason::None, true, false, false},
    {100, policy::TaskbarVisibility::Hidden, policy::PresentationVisibility::Clear, false,
     policy::SuppressionTransitionProfile::Default, policy::SuppressionReason::None, true, false, false},
    {349, policy::TaskbarVisibility::Hidden, policy::PresentationVisibility::Clear, false,
     policy::SuppressionTransitionProfile::Default, policy::SuppressionReason::None, true, false, false},
    {350, policy::TaskbarVisibility::Hidden, policy::PresentationVisibility::Clear, false,
     policy::SuppressionTransitionProfile::Default, policy::SuppressionReason::TaskbarHidden, false, true, true},
    {500, policy::TaskbarVisibility::Visible, policy::PresentationVisibility::Clear, false,
     policy::SuppressionTransitionProfile::Default, policy::SuppressionReason::TaskbarHidden, false, true, false},
    {999, policy::TaskbarVisibility::Visible, policy::PresentationVisibility::Clear, false,
     policy::SuppressionTransitionProfile::Default, policy::SuppressionReason::TaskbarHidden, false, true, false},
    {1000, policy::TaskbarVisibility::Visible, policy::PresentationVisibility::Clear, false,
     policy::SuppressionTransitionProfile::Default, policy::SuppressionReason::None, true, false, true},
};

struct TaskbarIdentityTraceStep {
    std::uint64_t now_ms;
    policy::TaskbarIdentity observed;
    policy::TaskbarIdentity expected_committed;
    bool expected_pending;
    bool expected_commit_changed;
};

constexpr TaskbarIdentityTraceStep kExplorerRestart[] = {
    {0, {1, 10}, {1, 10}, false, false},
    {100, {}, {1, 10}, true, false},
    {200, {2, 20}, {1, 10}, true, false},
    {449, {2, 20}, {1, 10}, true, false},
    {450, {2, 20}, {2, 20}, false, true},
    {500, {2, 20}, {2, 20}, false, false},
};

void ReplayExplorerRestartTrace() {
    policy::TaskbarIdentityState state{};
    state.committed = {1, 10};

    for (std::size_t index = 0; index < std::size(kExplorerRestart); ++index) {
        const auto& step = kExplorerRestart[index];
        const auto result = policy::ReduceTaskbarIdentity(
            state,
            step.observed,
            step.now_ms,
            250);
        state = result.state;

        CheckStep(
            state.committed == step.expected_committed,
            "explorer_restart",
            index,
            "unexpected committed taskbar identity");
        CheckStep(
            result.pending == step.expected_pending,
            "explorer_restart",
            index,
            "unexpected taskbar identity pending state");
        CheckStep(
            result.committed_changed == step.expected_commit_changed,
            "explorer_restart",
            index,
            "unexpected taskbar generation commit");
    }
}

struct RepairTraceStep {
    policy::OverlayRepairObservation observation;
    policy::OverlayRepairIntent expected;
};

constexpr RepairTraceStep kDisplayDpiRepair[] = {
    {{true, true, true, true, true, false}, {false, false, false, false, false}},
    {{true, true, true, true, false, false}, {false, false, true, true, false}},
    {{true, true, true, true, true, true}, {false, false, false, true, false}},
    {{true, true, false, true, true, false}, {false, false, true, false, false}},
    {{true, false, true, true, true, false}, {false, true, true, true, true}},
};

void ReplayDisplayDpiRepairTrace() {
    for (std::size_t index = 0; index < std::size(kDisplayDpiRepair); ++index) {
        const auto& step = kDisplayDpiRepair[index];
        const auto actual = policy::ComputeOverlayRepairIntent(step.observation);
        CheckStep(actual.ensure_exists == step.expected.ensure_exists, "display_dpi_repair", index, "unexpected create intent");
        CheckStep(actual.apply_style == step.expected.apply_style, "display_dpi_repair", index, "unexpected style intent");
        CheckStep(actual.reposition == step.expected.reposition, "display_dpi_repair", index, "unexpected reposition intent");
        CheckStep(actual.present == step.expected.present, "display_dpi_repair", index, "unexpected present intent");
        CheckStep(actual.commit_visible == step.expected.commit_visible, "display_dpi_repair", index, "unexpected visibility commit intent");
    }
}

void ReplayPresentationSequenceTrace() {
    constexpr std::uint64_t required_after_sequence = 10;
    CheckStep(
        !policy::HasNewPresentation(10, required_after_sequence),
        "present_before_show",
        0,
        "an existing presentation must not authorize a show commit");
    CheckStep(
        policy::HasNewPresentation(11, required_after_sequence),
        "present_before_show",
        1,
        "a new successful presentation should authorize a show commit");
}

}  // namespace

int main() {
    ReplaySuppressionTrace("powerpoint_slideshow_to_editor", kPowerPointSlideshowToEditor);
    ReplaySuppressionTrace("start_search_transient", kStartSearchTransient);
    ReplaySuppressionTrace("screenshot_freeze", kScreenshotFreeze);
    ReplaySuppressionTrace("taskbar_auto_hide", kTaskbarAutoHide);
    ReplayExplorerRestartTrace();
    ReplayDisplayDpiRepairTrace();
    ReplayPresentationSequenceTrace();

    if (failures != 0) {
        std::cerr << failures << " overlay policy trace test(s) failed\n";
        return 1;
    }
    std::cout << "overlay policy trace tests passed (7 fixtures)\n";
    return 0;
}
