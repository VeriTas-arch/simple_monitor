#pragma once

#include <cstdint>

namespace simple_monitor::overlay_policy {

enum class SuppressionReason {
    None,
    TaskbarHidden,
    FullscreenPresentation,
};

enum class TaskbarVisibility {
    Unknown,
    Hidden,
    Visible,
};

enum class PresentationVisibility {
    Unknown,
    Clear,
    Fullscreen,
};

enum class SuppressionTransitionProfile {
    Default,
    Fast,
};

enum class InitialOwnerBindingAction {
    None,
    Complete,
    Wait,
    Retry,
    Exhausted,
};

constexpr InitialOwnerBindingAction DecideInitialOwnerBindingAction(
    bool pending,
    bool overlay_valid,
    bool target_valid,
    bool owner_matches,
    unsigned attempts,
    unsigned max_attempts,
    std::uint64_t now_ms,
    std::uint64_t next_retry_ms) {
    if (!pending) {
        return InitialOwnerBindingAction::None;
    }
    if (!overlay_valid || !target_valid || now_ms < next_retry_ms) {
        return InitialOwnerBindingAction::Wait;
    }
    if (owner_matches) {
        return InitialOwnerBindingAction::Complete;
    }
    if (attempts >= max_attempts) {
        return InitialOwnerBindingAction::Exhausted;
    }
    return InitialOwnerBindingAction::Retry;
}

struct SuppressionObservation {
    bool known = false;
    SuppressionReason reason = SuppressionReason::None;
    SuppressionTransitionProfile transition_profile = SuppressionTransitionProfile::Default;
};

constexpr SuppressionObservation ResolveSuppressionObservation(
    TaskbarVisibility taskbar,
    bool screenshot_foreground,
    PresentationVisibility presentation) {
    if (screenshot_foreground) {
        return {};
    }
    if (taskbar == TaskbarVisibility::Unknown) {
        return {};
    }
    if (taskbar == TaskbarVisibility::Hidden) {
        return {true, SuppressionReason::TaskbarHidden};
    }
    if (presentation == PresentationVisibility::Unknown) {
        return {};
    }
    return {
        true,
        presentation == PresentationVisibility::Fullscreen
            ? SuppressionReason::FullscreenPresentation
            : SuppressionReason::None,
    };
}

struct SuppressionPolicyConfig {
    std::uint64_t enter_delay_ms = 250;
    std::uint64_t exit_delay_ms = 500;
    std::uint64_t fast_enter_delay_ms = 0;
    std::uint64_t fast_exit_delay_ms = 100;
};

struct SuppressionPolicyState {
    SuppressionReason committed = SuppressionReason::None;
    SuppressionReason candidate = SuppressionReason::None;
    std::uint64_t candidate_since_ms = 0;
    bool candidate_active = false;
    SuppressionTransitionProfile candidate_profile = SuppressionTransitionProfile::Default;
};

struct SuppressionPolicyResult {
    SuppressionPolicyState state;
    SuppressionReason previous_committed = SuppressionReason::None;
    std::uint64_t candidate_dwell_ms = 0;
    std::uint64_t required_delay_ms = 0;
    SuppressionTransitionProfile transition_profile = SuppressionTransitionProfile::Default;
    bool committed_changed = false;
};

constexpr SuppressionPolicyResult ReduceSuppressionPolicy(
    SuppressionPolicyState state,
    SuppressionObservation observation,
    std::uint64_t now_ms,
    SuppressionPolicyConfig config = {}) {
    SuppressionPolicyResult result{};
    result.state = state;
    result.previous_committed = state.committed;
    result.transition_profile = observation.transition_profile;

    if (!observation.known || observation.reason == state.committed) {
        result.state.candidate = state.committed;
        result.state.candidate_since_ms = 0;
        result.state.candidate_active = false;
        result.state.candidate_profile = SuppressionTransitionProfile::Default;
        return result;
    }

    const bool fast =
        observation.transition_profile == SuppressionTransitionProfile::Fast;
    result.required_delay_ms = observation.reason == SuppressionReason::None
        ? (fast ? config.fast_exit_delay_ms : config.exit_delay_ms)
        : (fast ? config.fast_enter_delay_ms : config.enter_delay_ms);

    if (!state.candidate_active ||
        state.candidate != observation.reason ||
        state.candidate_profile != observation.transition_profile) {
        result.state.candidate = observation.reason;
        result.state.candidate_since_ms = now_ms;
        result.state.candidate_active = true;
        result.state.candidate_profile = observation.transition_profile;
    } else {
        result.candidate_dwell_ms = now_ms - state.candidate_since_ms;
    }

    if (result.candidate_dwell_ms < result.required_delay_ms) {
        return result;
    }

    result.state.committed = observation.reason;
    result.state.candidate = observation.reason;
    result.state.candidate_since_ms = 0;
    result.state.candidate_active = false;
    result.state.candidate_profile = SuppressionTransitionProfile::Default;
    result.committed_changed = true;
    return result;
}

struct OverlayIntent {
    bool should_exist = false;
    bool should_be_visible = false;
    bool updates_frozen = false;
};

constexpr bool HasNewPresentation(
    std::uint64_t present_success_sequence,
    std::uint64_t required_after_sequence) {
    return present_success_sequence > required_after_sequence;
}

constexpr OverlayIntent ComputeOverlayIntent(
    bool taskbar_ready,
    SuppressionReason committed_suppression,
    bool screenshot_foreground) {
    return {
        taskbar_ready,
        taskbar_ready && committed_suppression == SuppressionReason::None,
        screenshot_foreground || committed_suppression != SuppressionReason::None,
    };
}

struct OverlayRepairObservation {
    bool valid = false;
    bool visible = false;
    bool topmost = false;
    bool layered = false;
    bool rect_valid = false;
    bool present_stale = false;
};

struct OverlayRepairIntent {
    bool ensure_exists = false;
    bool apply_style = false;
    bool reposition = false;
    bool present = false;
    bool commit_visible = false;
};

constexpr OverlayRepairIntent ComputeOverlayRepairIntent(
    OverlayRepairObservation observation) {
    if (!observation.valid || !observation.visible) {
        return {
            !observation.valid,
            true,
            true,
            true,
            true,
        };
    }

    const bool apply_style = !observation.layered;
    const bool reposition = !observation.topmost || !observation.rect_valid;
    return {
        false,
        apply_style,
        reposition,
        observation.present_stale || apply_style || !observation.rect_valid,
        false,
    };
}

struct TaskbarIdentity {
    std::uintptr_t hwnd = 0;
    std::uint32_t process_id = 0;
};

constexpr bool operator==(TaskbarIdentity left, TaskbarIdentity right) {
    return left.hwnd == right.hwnd && left.process_id == right.process_id;
}

constexpr bool operator!=(TaskbarIdentity left, TaskbarIdentity right) {
    return !(left == right);
}

struct TaskbarIdentityState {
    TaskbarIdentity committed;
    TaskbarIdentity candidate;
    std::uint64_t candidate_since_ms = 0;
    bool candidate_active = false;
};

struct TaskbarIdentityResult {
    TaskbarIdentityState state;
    TaskbarIdentity previous_committed;
    std::uint64_t candidate_dwell_ms = 0;
    bool committed_changed = false;
    bool pending = false;
};

constexpr TaskbarIdentityResult ReduceTaskbarIdentity(
    TaskbarIdentityState state,
    TaskbarIdentity observed,
    std::uint64_t now_ms,
    std::uint64_t settle_delay_ms) {
    TaskbarIdentityResult result{};
    result.state = state;
    result.previous_committed = state.committed;

    if (observed.hwnd == 0 || observed.process_id == 0) {
        result.state.candidate = state.committed;
        result.state.candidate_since_ms = 0;
        result.state.candidate_active = false;
        result.pending = true;
        return result;
    }

    if (observed == state.committed) {
        result.state.candidate = state.committed;
        result.state.candidate_since_ms = 0;
        result.state.candidate_active = false;
        return result;
    }

    result.pending = true;
    if (!state.candidate_active || state.candidate != observed) {
        result.state.candidate = observed;
        result.state.candidate_since_ms = now_ms;
        result.state.candidate_active = true;
        return result;
    }

    result.candidate_dwell_ms = now_ms - state.candidate_since_ms;
    if (result.candidate_dwell_ms < settle_delay_ms) {
        return result;
    }

    result.state.committed = observed;
    result.state.candidate = observed;
    result.state.candidate_since_ms = 0;
    result.state.candidate_active = false;
    result.committed_changed = true;
    result.pending = false;
    return result;
}

}  // namespace simple_monitor::overlay_policy
