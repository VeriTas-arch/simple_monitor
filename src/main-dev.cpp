#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <objbase.h>
#include <oleacc.h>

#include "app_support.h"
#include "logging.h"
#include "metric_refresh_policy.h"
#include "overlay_policy.h"
#include "win32_function.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <string>
#include <vector>

namespace {

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// Core constants, message IDs, and long-lived application state.
constexpr wchar_t kControllerWindowClass[] = L"SimpleMonitorDevControllerWindow";
constexpr wchar_t kOverlayWindowClass[] = L"SimpleMonitorOverlayWindow";
constexpr wchar_t kRunValue[] = L"SimpleMonitorDev";

// Timer roles:
// - refresh: sample metrics and repaint the overlay.
// - placement: keep the overlay aligned to the taskbar tray anchor.
// - state: reconcile taskbar ownership, visibility, screenshot, and key-state changes.
// - startup init: defer expensive monitor setup when launched at logon.
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT_PTR kPlacementTimer = 2;
constexpr UINT_PTR kStateTimer = 3;
constexpr UINT_PTR kStartupInitTimer = 5;
constexpr UINT kStateIntervalMs = 100;
constexpr UINT kPlacementIntervalMs = 5000;
constexpr UINT kStartupInitDelayMs = 8000;
constexpr DWORD kPdhRetryIntervalMs = 5000;
constexpr ULONGLONG kStartupPerformanceSettledMs = 30000;
constexpr ULONGLONG kStartupPerformanceLogMaxBytes = 256ULL * 1024ULL;
constexpr wchar_t kStartupPerformanceLogName[] = L"startup-perf-dev.log";
constexpr DWORD kHealthLogIntervalMs = 60000;
constexpr DWORD kOverlayRepairIntervalMs = 5000;
constexpr DWORD kPresentStaleThresholdMs = 5000;
constexpr std::uint64_t kInitialOwnerBindingRetryIntervalMs = 500;
constexpr unsigned kInitialOwnerBindingMaxAttempts = 8;
constexpr std::uint64_t kTaskViewStabilizeDelayMs = 500;
constexpr std::uint64_t kTaskbarIdentitySettleMs = 250;
constexpr simple_monitor::overlay_policy::SuppressionPolicyConfig kSuppressionPolicyConfig{
    250,
    500,
    0,
    100,
};
constexpr unsigned kFailureLogIntervalMs = 30000;
constexpr int kMinimumTaskbarVisibleDip = 8;
constexpr int kFullscreenCoverageToleranceDip = 2;
constexpr UINT kTrayIconId = 1;
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_TRAY_LAYOUT_CHANGED = WM_APP + 2;
constexpr UINT WM_RECONCILE = WM_APP + 3;
constexpr UINT WM_INITIALIZE_METRICS = WM_APP + 4;
constexpr UINT WM_FOREGROUND_CHANGED = WM_APP + 5;
constexpr int kAppIconResource = 101;

enum MenuId : UINT {
    ID_CLICK_THROUGH = 1001,
    ID_STARTUP = 1002,
    ID_EXIT = 1003,
    ID_OPEN_CONFIG = 1004,
    ID_RELOAD_CONFIG = 1005,
    ID_RECOVER_TASKBAR = 1006,
};

using simple_monitor::overlay_policy::ComputeOverlayIntent;
using simple_monitor::overlay_policy::ComputeOverlayRepairIntent;
using simple_monitor::overlay_policy::DecideInitialOwnerBindingAction;
using simple_monitor::overlay_policy::HasNewPresentation;
using simple_monitor::overlay_policy::InitialOwnerBindingAction;
using simple_monitor::overlay_policy::OverlayIntent;
using simple_monitor::overlay_policy::OverlayRepairObservation;
using simple_monitor::overlay_policy::PresentationVisibility;
using simple_monitor::overlay_policy::ReduceSuppressionPolicy;
using simple_monitor::overlay_policy::ReduceTaskbarIdentity;
using simple_monitor::overlay_policy::ResolveSuppressionObservation;
using simple_monitor::overlay_policy::ShouldCompleteTaskViewTransition;
using simple_monitor::overlay_policy::ShouldPromoteOverlayDuringScreenshotResume;
using simple_monitor::overlay_policy::ShouldPromoteOverlayForTaskViewTransition;
using simple_monitor::overlay_policy::ShouldRunTaskViewStabilization;
using simple_monitor::overlay_policy::SuppressionObservation;
using simple_monitor::overlay_policy::SuppressionPolicyState;
using simple_monitor::overlay_policy::SuppressionReason;
using simple_monitor::overlay_policy::SuppressionTransitionProfile;
using simple_monitor::overlay_policy::TaskbarIdentity;
using simple_monitor::overlay_policy::TaskbarIdentityState;
using simple_monitor::overlay_policy::TaskbarVisibility;

enum OverlayReconcileFlag : unsigned {
    OverlayReconcileNone = 0,
    OverlayReconcileSampleMetrics = 1U << 0,
    OverlayReconcileSampleKeys = 1U << 1,
    OverlayReconcileReposition = 1U << 2,
    OverlayReconcileRender = 1U << 3,
    OverlayReconcileResetSurface = 1U << 4,
    OverlayReconcileRefreshTrayIcon = 1U << 5,
    OverlayReconcileApplyStyle = 1U << 6,
    OverlayReconcilePromoteForTaskView = 1U << 7,
};

enum class PreferredAppMode {
    Default,
    AllowDark,
    ForceDark,
    ForceLight,
    Max,
};

struct CpuSampler {
    ULONGLONG idle = 0;
    ULONGLONG kernel = 0;
    ULONGLONG user = 0;
    bool has_sample = false;
    double percent = 0.0;
};

struct NetworkSampler {
    struct InterfaceSample {
        uint64_t luid = 0;
        uint64_t in_bytes = 0;
        uint64_t out_bytes = 0;
        bool seen = false;
    };

    std::vector<InterfaceSample> interfaces;
    DWORD tick = 0;
    bool has_sample = false;
    bool availability_known = false;
    bool available = false;
    double down_bps = 0.0;
    double up_bps = 0.0;
};

struct PdhGroup {
    HQUERY query = nullptr;
    HCOUNTER counter = nullptr;
    bool ready = false;
    bool needs_second_sample = false;
    bool availability_known = false;
    bool provider_available = false;
    bool wildcard_array = false;
    bool instance_count_known = false;
    bool instance_inventory_logged = false;
    double value = -1.0;
    std::wstring name;
    std::wstring wildcard_path;
    std::vector<BYTE> formatted_buffer;
    DWORD last_init_attempt_tick = 0;
    DWORD last_instance_count = 0;
    ULONGLONG last_init_duration_ms = 0;
};

struct Metrics {
    double cpu = 0.0;
    DWORD memory_load = 0;
    double down_bps = 0.0;
    double up_bps = 0.0;
    double gpu = -1.0;
    double disk = -1.0;
    bool caps = false;
    bool insert = false;
    bool num = false;
};

struct RenderResources {
    ID2D1Factory* d2d_factory = nullptr;
    IDWriteFactory* dwrite_factory = nullptr;
    IWICImagingFactory* wic_factory = nullptr;
    IDWriteTextFormat* text_format = nullptr;
    IDWriteTextFormat* arrow_text_format = nullptr;
    IDWriteTextFormat* key_text_format = nullptr;
    UINT text_format_dpi = 0;
    int text_format_font_size_dip = 0;
    int arrow_text_format_font_size_dip = 0;
    int key_text_format_font_size_dip = 0;
};

struct WindowState {
    HINSTANCE instance = nullptr;
    HWND controller_hwnd = nullptr;
    HWND overlay_hwnd = nullptr;
    UINT taskbar_created = 0;
    UINT dpi = 96;
    const wchar_t* shutdown_trigger = L"window_destroy";
    bool com_initialized = false;
    bool launched_at_startup = false;
    bool monitor_initialized = false;
    bool overlay_destroy_expected = false;
};

enum OverlayInvariantIssue : unsigned {
    OverlayInvariantNone = 0,
    OverlayInvariantMissing = 1U << 0,
    OverlayInvariantHidden = 1U << 1,
    OverlayInvariantNotTopmost = 1U << 2,
    OverlayInvariantOwnerMismatch = 1U << 3,
    OverlayInvariantMissingLayeredStyle = 1U << 4,
    OverlayInvariantInvalidRect = 1U << 5,
    OverlayInvariantPresentStale = 1U << 6,
    OverlayInvariantCloaked = 1U << 7,
};

struct TrayState {
    bool click_through = false;
    std::vector<HWINEVENTHOOK> event_hooks;
};

struct PlacementState {
    HWND taskbar_owner = nullptr;
    TaskbarIdentityState taskbar_identity;
    LONG tray_layout_update_pending = 0;
    RECT last_logged_taskbar_rect{};
    RECT last_logged_anchor_rect{};
    RECT last_logged_overlay_rect{};
    bool has_last_logged_taskbar_rect = false;
    bool has_last_logged_anchor_rect = false;
    bool has_last_logged_overlay_rect = false;
    int last_logged_anchor_mode = -1;
};

struct SuppressionState {
    SuppressionPolicyState policy;
    bool overlay_update_frozen = false;
    DWORD suppression_started_tick = 0;
    DWORD refresh_resume_tick = 0;
    std::uint64_t decision_sequence = 0;
};

struct ReconcileState {
    struct InitialOwnerBindingState {
        HWND target = nullptr;
        std::uint64_t next_retry_ms = 0;
        unsigned attempts = 0;
        bool pending = false;
        bool exhausted = false;
    } initial_owner_binding;

    const wchar_t* pending_trigger = L"coalesced";
    unsigned pending_flags = OverlayReconcileNone;
    std::wstring queued_trigger = L"queued";
    unsigned queued_flags = OverlayReconcileNone;
    unsigned deferred_flags = OverlayReconcileNone;
    DWORD next_destroy_retry_tick = 0;
    DWORD next_visibility_retry_tick = 0;
    bool recreate_pending = false;
    bool visibility_retry_pending = false;
    bool active = false;
    bool pending = false;
    bool message_queued = false;
    bool task_view_transition_pending = false;
    bool task_view_stabilize_pending = false;
    std::uint64_t task_view_stabilize_after_ms = 0;
};

struct MetricsState {
    CpuSampler cpu;
    NetworkSampler network;
    PdhGroup gpu;
    PdhGroup disk;
    Metrics current;
    bool providers_initialized = false;
    bool initialization_pending = false;
};

struct RenderState {
    RenderResources resources;
    int last_frame_width = 0;
    int last_frame_height = 0;
};

struct DiagnosticsState {
    DWORD last_health_log_tick = 0;
    DWORD last_state_timer_tick = 0;
    DWORD last_state_timer_gap_ms = 0;
    DWORD last_metrics_sample_tick = 0;
    DWORD last_reposition_success_tick = 0;
    DWORD last_render_attempt_tick = 0;
    DWORD last_render_success_tick = 0;
    DWORD last_present_attempt_tick = 0;
    DWORD last_present_success_tick = 0;
    std::uint64_t present_success_sequence = 0;
    DWORD overlay_created_tick = 0;
    DWORD last_overlay_repair_tick = 0;
    DWORD last_overlay_detail_log_tick = 0;
    std::uint64_t overlay_generation = 0;
    unsigned present_failures = 0;
    unsigned render_failures = 0;
    unsigned overlay_repairs = 0;
    unsigned overlay_repair_failures = 0;
    unsigned overlay_refreshes = 0;
    unsigned overlay_refresh_failures = 0;
    bool last_frame_has_visible_pixels = false;
};

struct StartupPerformanceState {
    ULONGLONG process_entry_tick = 0;
    bool settled_recorded = false;
};

struct AppState {
    WindowState window;
    TrayState tray;
    PlacementState placement;
    SuppressionState suppression;
    ReconcileState reconcile;
    MetricsState metrics;
    RenderState render;
    DiagnosticsState diagnostics;
    StartupPerformanceState startup_performance;
    simple_monitor::Config config;
};

AppState g_app;

using simple_monitor::ConfigPath;
using simple_monitor::LogDebug;
using simple_monitor::LogError;
using simple_monitor::LogErrorRateLimited;
using simple_monitor::LogFailureRecovered;
using simple_monitor::LogInfo;
using simple_monitor::LogLevel;
using simple_monitor::LogWarning;
using simple_monitor::LogWarningRateLimited;
using simple_monitor::ModuleDir;
using simple_monitor::metric_refresh_policy::AggregateBusiestGpuEngine;
using simple_monitor::metric_refresh_policy::ClassifyPdhSample;
using simple_monitor::metric_refresh_policy::GpuEngineSampleView;
using simple_monitor::metric_refresh_policy::PdhSampleDisposition;
using simple_monitor::metric_refresh_policy::ShouldReinitializePdhGroup;

// Forward declarations for cross-section entry points.
void RenderOverlay(HWND hwnd);
void ResetLayeredSurface(HWND hwnd);
bool EnsureOverlayWindow(DWORD* error_out = nullptr);
bool DestroyOverlayWindow(const wchar_t* reason);
bool PrepareOverlayForShow(HWND hwnd, const wchar_t* trigger, bool reset_surface);
void ReconcileOverlayState(
    const wchar_t* trigger,
    unsigned flags = OverlayReconcileNone);
void RequestOverlayReconcile(
    const wchar_t* trigger,
    unsigned flags = OverlayReconcileNone);

// Shared utility helpers.
ULONGLONG FileTimeToU64(const FILETIME& ft) {
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

int Scale(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

template <typename T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

UINT WindowDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static auto get_dpi_for_window =
        simple_monitor::LoadOptionalFunction<GetDpiForWindowFn>(
            GetModuleHandleW(L"user32.dll"),
            "GetDpiForWindow");

    if (get_dpi_for_window) {
        UINT dpi = get_dpi_for_window(hwnd);
        if (dpi != 0) {
            return dpi;
        }
    }

    HDC screen = GetDC(hwnd);
    const UINT dpi = screen ? static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX)) : 96;
    if (screen) {
        ReleaseDC(hwnd, screen);
    }
    return dpi == 0 ? 96 : dpi;
}

double ClampPercent(double value) {
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 100.0) {
        return 100.0;
    }
    return value;
}

bool RectEquals(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool TickPassed(DWORD now, DWORD deadline) {
    return static_cast<LONG>(now - deadline) >= 0;
}

long long TickAgeMs(DWORD now, DWORD tick) {
    return tick == 0 ? -1 : static_cast<long long>(now - tick);
}

bool CommandLineHasFlag(const wchar_t* flag) {
    return flag && std::wcsstr(GetCommandLineW(), flag) != nullptr;
}

void EnableSystemMenuTheme() {
    HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
    if (!uxtheme) {
        return;
    }

    using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
    using FlushMenuThemesFn = void(WINAPI*)();

    auto set_preferred_app_mode = simple_monitor::LoadOptionalFunction<SetPreferredAppModeFn>(
        uxtheme,
        MAKEINTRESOURCEA(135));
    auto flush_menu_themes = simple_monitor::LoadOptionalFunction<FlushMenuThemesFn>(
        uxtheme,
        MAKEINTRESOURCEA(136));

    if (set_preferred_app_mode) {
        set_preferred_app_mode(PreferredAppMode::AllowDark);
    }
    if (flush_menu_themes) {
        flush_menu_themes();
    }
}

// Configuration.
void ApplyAppConfig(const simple_monitor::Config& config) {
    g_app.config = config;
    simple_monitor::ConfigureLogging(
        g_app.config.debug_log,
        simple_monitor::DebugLogPath(L"debug-dev.log"),
        simple_monitor::ParseLogLevel(g_app.config.log_level));
}

void LoadAppConfig() {
    ApplyAppConfig(simple_monitor::LoadConfig());
}

void LogLoggingConfigChange(
    const simple_monitor::Config& previous,
    const simple_monitor::Config& next) {
    if (!previous.debug_log ||
        (previous.debug_log == next.debug_log && previous.log_level == next.log_level)) {
        return;
    }

    wchar_t message[512]{};
    _snwprintf_s(
        message,
        _countof(message),
        _TRUNCATE,
        L"event=logging_config_changed trigger=config_reload old_enabled=%d new_enabled=%d "
        L"old_level=%ls new_level=%ls",
        previous.debug_log ? 1 : 0,
        next.debug_log ? 1 : 0,
        previous.log_level.c_str(),
        next.log_level.c_str());

    if (previous.log_level == L"error") {
        LogError(L"%ls", message);
    } else if (previous.log_level == L"warning") {
        LogWarning(L"%ls", message);
    } else {
        LogInfo(L"%ls", message);
    }
}

void LogConfigSnapshot(const wchar_t* event_name) {
    const std::wstring path = ConfigPath();
    const DWORD attributes = GetFileAttributesW(path.c_str());
    LogInfo(
        L"event=%ls path=\"%ls\" exists=%d debug_log=%d log_level=%ls content_padding_x=%d column_gap=%d "
        L"gap_after_network=%d gap_after_system=%d gap_after_disk=%d offset_right=%d font_size=%d "
        L"key_font_size=%d network_arrow_style=%ls network_arrow_font_size=%d network_arrow_gap=%d "
        L"show_key_widget=%d",
        event_name,
        path.c_str(),
        attributes != INVALID_FILE_ATTRIBUTES ? 1 : 0,
        g_app.config.debug_log ? 1 : 0,
        g_app.config.log_level.c_str(),
        g_app.config.content_padding_x_dip,
        g_app.config.column_gap_dip,
        g_app.config.gap_after_network_dip,
        g_app.config.gap_after_system_dip,
        g_app.config.gap_after_disk_dip,
        g_app.config.offset_right_dip,
        g_app.config.font_size_dip,
        g_app.config.key_font_size_dip,
        g_app.config.network_arrow_style.c_str(),
        g_app.config.network_arrow_font_size_dip,
        g_app.config.network_arrow_gap_dip,
        g_app.config.show_key_widget ? 1 : 0);
}

struct ProcessPerformanceSnapshot {
    ULONGLONG kernel_100ns = 0;
    ULONGLONG user_100ns = 0;
    IO_COUNTERS io{};
    bool cpu_valid = false;
    bool io_valid = false;
};

ULONGLONG FileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

ProcessPerformanceSnapshot CaptureProcessPerformance() {
    ProcessPerformanceSnapshot snapshot;
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    snapshot.cpu_valid = GetProcessTimes(
        GetCurrentProcess(),
        &creation,
        &exit,
        &kernel,
        &user) != FALSE;
    if (snapshot.cpu_valid) {
        snapshot.kernel_100ns = FileTimeValue(kernel);
        snapshot.user_100ns = FileTimeValue(user);
    }
    snapshot.io_valid = GetProcessIoCounters(GetCurrentProcess(), &snapshot.io) != FALSE;
    return snapshot;
}

void AppendStartupPerformanceLine(const wchar_t* line) {
    const std::wstring path = simple_monitor::DebugLogPath(kStartupPerformanceLogName);
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        ULARGE_INTEGER size{};
        size.HighPart = attributes.nFileSizeHigh;
        size.LowPart = attributes.nFileSizeLow;
        if (size.QuadPart >= kStartupPerformanceLogMaxBytes) {
            const std::wstring backup_path = path + L".1";
            if (!MoveFileExW(
                    path.c_str(),
                    backup_path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                LogWarningRateLimited(
                    L"startup_performance.persist",
                    kFailureLogIntervalMs,
                    L"event=startup_performance_log_failed stage=rotate error=%lu",
                    GetLastError());
            }
        }
    }

    HANDLE file = CreateFileW(
        path.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        LogWarningRateLimited(
            L"startup_performance.persist",
            kFailureLogIntervalMs,
            L"event=startup_performance_log_failed stage=open error=%lu",
            GetLastError());
        return;
    }

    std::wstring record(line ? line : L"");
    record += L"\r\n";
    LARGE_INTEGER file_size{};
    if (GetFileSizeEx(file, &file_size) && file_size.QuadPart == 0) {
        const wchar_t byte_order_mark = 0xFEFF;
        DWORD bom_written = 0;
        if (!WriteFile(
                file,
                &byte_order_mark,
                sizeof(byte_order_mark),
                &bom_written,
                nullptr) ||
            bom_written != sizeof(byte_order_mark)) {
            const DWORD error = GetLastError();
            CloseHandle(file);
            LogWarningRateLimited(
                L"startup_performance.persist",
                kFailureLogIntervalMs,
                L"event=startup_performance_log_failed stage=write_bom error=%lu",
                error);
            return;
        }
    }

    const DWORD expected_bytes = static_cast<DWORD>(record.size() * sizeof(wchar_t));
    DWORD written = 0;
    const BOOL write_result = WriteFile(
        file,
        record.data(),
        expected_bytes,
        &written,
        nullptr);
    const DWORD write_error = write_result ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!write_result || written != expected_bytes) {
        LogWarningRateLimited(
            L"startup_performance.persist",
            kFailureLogIntervalMs,
            L"event=startup_performance_log_failed stage=write error=%lu written=%lu expected=%lu",
            write_error,
            written,
            expected_bytes);
    }
}

void RecordStartupPerformance(const wchar_t* stage) {
    const ProcessPerformanceSnapshot snapshot = CaptureProcessPerformance();
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG wall_ms = g_app.startup_performance.process_entry_tick == 0
        ? 0
        : now - g_app.startup_performance.process_entry_tick;
    const ULONGLONG kernel_ms = snapshot.kernel_100ns / 10000ULL;
    const ULONGLONG user_ms = snapshot.user_100ns / 10000ULL;
    const ULONGLONG cpu_ms = kernel_ms + user_ms;
    const wchar_t* gpu_mode = g_app.metrics.gpu.wildcard_path.empty()
        ? L"pending"
        : (g_app.metrics.gpu.wildcard_array ? L"wildcard_array" : L"scalar");

    SYSTEMTIME local_time{};
    GetLocalTime(&local_time);
    wchar_t line[1024]{};
    const int length = _snwprintf_s(
        line,
        _countof(line),
        _TRUNCATE,
        L"time=%04u-%02u-%02uT%02u:%02u:%02u.%03u event=startup_performance schema=1 build=dev "
        L"pid=%lu stage=%ls launch=%ls wall_ms=%llu cpu_valid=%d cpu_ms=%llu kernel_ms=%llu user_ms=%llu "
        L"io_valid=%d read_bytes=%llu write_bytes=%llu other_bytes=%llu gpu_mode=%ls gpu_handles=%d "
        L"gpu_instances_known=%d gpu_instances=%lu gpu_init_ms=%llu "
        L"disk_init_ms=%llu",
        static_cast<unsigned>(local_time.wYear),
        static_cast<unsigned>(local_time.wMonth),
        static_cast<unsigned>(local_time.wDay),
        static_cast<unsigned>(local_time.wHour),
        static_cast<unsigned>(local_time.wMinute),
        static_cast<unsigned>(local_time.wSecond),
        static_cast<unsigned>(local_time.wMilliseconds),
        GetCurrentProcessId(),
        stage,
        g_app.window.launched_at_startup ? L"startup" : L"manual",
        static_cast<unsigned long long>(wall_ms),
        snapshot.cpu_valid ? 1 : 0,
        static_cast<unsigned long long>(cpu_ms),
        static_cast<unsigned long long>(kernel_ms),
        static_cast<unsigned long long>(user_ms),
        snapshot.io_valid ? 1 : 0,
        static_cast<unsigned long long>(snapshot.io.ReadTransferCount),
        static_cast<unsigned long long>(snapshot.io.WriteTransferCount),
        static_cast<unsigned long long>(snapshot.io.OtherTransferCount),
        gpu_mode,
        g_app.metrics.gpu.counter ? 1 : 0,
        g_app.metrics.gpu.instance_count_known ? 1 : 0,
        g_app.metrics.gpu.last_instance_count,
        static_cast<unsigned long long>(g_app.metrics.gpu.last_init_duration_ms),
        static_cast<unsigned long long>(g_app.metrics.disk.last_init_duration_ms));
    if (length > 0) {
        LogInfo(L"%ls", line);
        if (g_app.window.launched_at_startup) {
            AppendStartupPerformanceLine(line);
        }
    } else {
        LogWarningRateLimited(
            L"startup_performance.format",
            kFailureLogIntervalMs,
            L"event=startup_performance_log_failed stage=format");
    }
}

void MaybeRecordStartupPerformanceSettled() {
    if (g_app.startup_performance.settled_recorded ||
        g_app.startup_performance.process_entry_tick == 0 ||
        GetTickCount64() - g_app.startup_performance.process_entry_tick <
            kStartupPerformanceSettledMs) {
        return;
    }

    g_app.startup_performance.settled_recorded = true;
    RecordStartupPerformance(L"settled");
}

// Startup integration.
bool IsStartupEnabled() {
    return simple_monitor::IsStartupEnabled(kRunValue);
}

bool SetStartupEnabled(bool enabled) {
    const bool succeeded = simple_monitor::SetStartupEnabled(kRunValue, enabled);
    if (!succeeded) {
        LogError(L"event=startup_setting_failed enabled=%d", enabled ? 1 : 0);
    }
    return succeeded;
}

// Foreground, fullscreen, and suppression detection.
std::wstring Basename(std::wstring path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        path.erase(0, slash + 1);
    }

    for (wchar_t& ch : path) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return path;
}

std::wstring WindowProcessBasename(HWND hwnd) {
    DWORD process_id = 0;
    GetWindowThreadProcessId(hwnd, &process_id);
    if (process_id == 0) {
        return L"";
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) {
        return L"";
    }

    std::wstring path(MAX_PATH, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    const bool ok = QueryFullProcessImageNameW(process, 0, path.data(), &size) != 0;
    CloseHandle(process);
    if (!ok) {
        return L"";
    }

    path.resize(size);
    return Basename(path);
}

bool WindowClassIs(HWND hwnd, const wchar_t* class_name) {
    wchar_t current_class[128]{};
    return GetClassNameW(hwnd, current_class, ARRAYSIZE(current_class)) != 0 &&
           std::wcscmp(current_class, class_name) == 0;
}

std::wstring WindowClassName(HWND hwnd) {
    wchar_t class_name[128]{};
    if (!hwnd || GetClassNameW(hwnd, class_name, ARRAYSIZE(class_name)) == 0) {
        return L"";
    }
    return class_name;
}

std::wstring WindowTitleForLog(HWND hwnd) {
    wchar_t title[160]{};
    const int length = hwnd ? GetWindowTextW(hwnd, title, ARRAYSIZE(title)) : 0;
    if (length <= 0) {
        return L"";
    }

    std::wstring result(title, static_cast<size_t>(length));
    for (wchar_t& ch : result) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
            ch = L' ';
        } else if (ch == L'"') {
            ch = L'\'';
        }
    }
    return result;
}

bool IsTaskbarWindowClass(HWND hwnd) {
    return WindowClassIs(hwnd, L"Shell_TrayWnd") ||
           WindowClassIs(hwnd, L"Shell_SecondaryTrayWnd");
}

bool IsTaskbarPreviewWindow(HWND hwnd) {
    // Taskbar previews and Task View can cover the monitor without representing
    // a fullscreen application.
    return WindowClassIs(hwnd, L"TaskListThumbnailWnd") ||
           WindowClassIs(hwnd, L"XamlExplorerHostIslandWindow");
}

bool IsTaskViewWindow(HWND hwnd) {
    return WindowClassIs(hwnd, L"XamlExplorerHostIslandWindow") &&
           WindowProcessBasename(hwnd) == L"explorer.exe";
}

bool IsBuiltinScreenshotForeground(HWND foreground) {
    if (!foreground || foreground == g_app.window.overlay_hwnd) {
        return false;
    }

    const std::wstring exe = WindowProcessBasename(foreground);
    return exe == L"snippingtool.exe" ||
           exe == L"screenclippinghost.exe" ||
           exe == L"snipandsketch.exe";
}

HWND TaskbarWindow() {
    return FindWindowW(L"Shell_TrayWnd", nullptr);
}

DWORD WindowProcessId(HWND hwnd) {
    DWORD process_id = 0;
    if (hwnd) {
        GetWindowThreadProcessId(hwnd, &process_id);
    }
    return process_id;
}

TaskbarIdentity CurrentTaskbarIdentity() {
    HWND taskbar = TaskbarWindow();
    return {
        reinterpret_cast<std::uintptr_t>(taskbar),
        static_cast<std::uint32_t>(WindowProcessId(taskbar)),
    };
}

HWND TaskbarIdentityWindow(TaskbarIdentity identity) {
    return reinterpret_cast<HWND>(identity.hwnd);
}

HWND CommittedTaskbarWindow() {
    return TaskbarIdentityWindow(g_app.placement.taskbar_identity.committed);
}

DWORD CommittedTaskbarProcessId() {
    return static_cast<DWORD>(g_app.placement.taskbar_identity.committed.process_id);
}

TaskbarVisibility ObserveTaskbarVisibility() {
    HWND taskbar = CommittedTaskbarWindow();
    if (!taskbar || !IsWindow(taskbar)) {
        return TaskbarVisibility::Unknown;
    }
    if (!IsWindowVisible(taskbar) || IsIconic(taskbar)) {
        return TaskbarVisibility::Hidden;
    }

    RECT rect{};
    if (!GetWindowRect(taskbar, &rect)) {
        return TaskbarVisibility::Unknown;
    }

    HMONITOR monitor = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (!monitor || !GetMonitorInfoW(monitor, &monitor_info)) {
        return TaskbarVisibility::Unknown;
    }

    RECT visible_rect{};
    if (!IntersectRect(&visible_rect, &rect, &monitor_info.rcMonitor)) {
        return TaskbarVisibility::Hidden;
    }

    const int minimum_visible_size = Scale(kMinimumTaskbarVisibleDip, WindowDpi(taskbar));
    const bool visible =
        visible_rect.right - visible_rect.left >= minimum_visible_size &&
        visible_rect.bottom - visible_rect.top >= minimum_visible_size;
    return visible ? TaskbarVisibility::Visible : TaskbarVisibility::Hidden;
}

bool IsTaskbarRelatedWindow(HWND hwnd) {
    if (!hwnd) {
        return false;
    }

    if (IsTaskbarWindowClass(hwnd)) {
        return true;
    }

    HWND taskbar = TaskbarWindow();
    if (!taskbar) {
        return false;
    }

    if (hwnd == taskbar) {
        return true;
    }

    if (IsChild(taskbar, hwnd) != 0 ||
        GetAncestor(hwnd, GA_ROOT) == taskbar ||
        GetAncestor(hwnd, GA_ROOTOWNER) == taskbar) {
        return true;
    }

    for (HWND owner = GetWindow(hwnd, GW_OWNER); owner; owner = GetWindow(owner, GW_OWNER)) {
        if (owner == taskbar || IsTaskbarWindowClass(owner)) {
            return true;
        }
    }

    return false;
}

enum class MonitorCoverageObservation {
    Unknown,
    Clear,
    Covers,
};

struct ForegroundCoverageObservation {
    MonitorCoverageObservation coverage = MonitorCoverageObservation::Unknown;
    bool power_point_slideshow = false;
};

struct PresentationObservation {
    PresentationVisibility visibility = PresentationVisibility::Unknown;
    SuppressionTransitionProfile transition_profile = SuppressionTransitionProfile::Default;
};

MonitorCoverageObservation ObserveWindowMonitorCoverage(HWND window) {
    RECT window_rect{};
    if (!GetWindowRect(window, &window_rect)) {
        return MonitorCoverageObservation::Unknown;
    }

    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (!monitor || !GetMonitorInfoW(monitor, &monitor_info)) {
        return MonitorCoverageObservation::Unknown;
    }

    const int tolerance = Scale(kFullscreenCoverageToleranceDip, WindowDpi(window));
    const bool covers =
        window_rect.left <= monitor_info.rcMonitor.left + tolerance &&
        window_rect.top <= monitor_info.rcMonitor.top + tolerance &&
        window_rect.right >= monitor_info.rcMonitor.right - tolerance &&
        window_rect.bottom >= monitor_info.rcMonitor.bottom - tolerance;
    return covers ? MonitorCoverageObservation::Covers : MonitorCoverageObservation::Clear;
}

ForegroundCoverageObservation ObserveForegroundMonitorCoverage(HWND foreground) {
    if (!foreground || !IsWindow(foreground)) {
        return {};
    }
    if (foreground == g_app.window.overlay_hwnd ||
        foreground == g_app.window.controller_hwnd ||
        IsTaskbarPreviewWindow(foreground) ||
        !IsWindowVisible(foreground) ||
        IsIconic(foreground)) {
        return {MonitorCoverageObservation::Clear, false};
    }

    const DWORD foreground_process_id = WindowProcessId(foreground);
    if (foreground_process_id == 0) {
        return {};
    }
    HWND candidate = GetAncestor(foreground, GA_ROOT);
    if (!candidate) {
        return {};
    }
    bool unknown_observed = false;
    for (unsigned depth = 0; candidate && depth < 8; ++depth) {
        if (!IsWindow(candidate)) {
            unknown_observed = true;
            break;
        }
        if (candidate != g_app.window.overlay_hwnd &&
            candidate != g_app.window.controller_hwnd &&
            !IsTaskbarRelatedWindow(candidate) &&
            !IsTaskbarPreviewWindow(candidate) &&
            IsWindowVisible(candidate) &&
            !IsIconic(candidate)) {
            const DWORD candidate_process_id = WindowProcessId(candidate);
            if (candidate_process_id == 0) {
                unknown_observed = true;
            } else if (candidate_process_id == foreground_process_id) {
                const MonitorCoverageObservation coverage =
                    ObserveWindowMonitorCoverage(candidate);
                if (coverage == MonitorCoverageObservation::Covers) {
                    const bool power_point_slideshow =
                        WindowClassIs(candidate, L"screenClass") &&
                        WindowProcessBasename(candidate) == L"powerpnt.exe";
                    return {
                        MonitorCoverageObservation::Covers,
                        power_point_slideshow,
                    };
                }
                unknown_observed |= coverage == MonitorCoverageObservation::Unknown;
            }
        }

        HWND owner = GetWindow(candidate, GW_OWNER);
        HWND next = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
        if (owner && !next) {
            unknown_observed = true;
            break;
        }
        if (next == candidate) {
            break;
        }
        candidate = next;
    }
    return {
        unknown_observed
            ? MonitorCoverageObservation::Unknown
            : MonitorCoverageObservation::Clear,
        false,
    };
}

bool IsPowerPointEditorMainWindow(HWND foreground) {
    return foreground &&
           GetAncestor(foreground, GA_ROOT) == foreground &&
           GetWindow(foreground, GW_OWNER) == nullptr &&
           WindowClassIs(foreground, L"PPTFrameClass") &&
           WindowProcessBasename(foreground) == L"powerpnt.exe";
}

PresentationObservation ObservePresentation(
    HWND foreground,
    SuppressionReason committed_suppression) {
    QUERY_USER_NOTIFICATION_STATE state = QUNS_NOT_PRESENT;
    if (FAILED(SHQueryUserNotificationState(&state))) {
        return {};
    }

    const bool may_suppress =
        state == QUNS_BUSY ||
        state == QUNS_RUNNING_D3D_FULL_SCREEN ||
        state == QUNS_PRESENTATION_MODE;
    if (!may_suppress) {
        if (committed_suppression != SuppressionReason::FullscreenPresentation ||
            WindowProcessBasename(foreground) != L"powerpnt.exe") {
            return {PresentationVisibility::Clear, SuppressionTransitionProfile::Default};
        }

        const ForegroundCoverageObservation coverage =
            ObserveForegroundMonitorCoverage(foreground);
        if (coverage.power_point_slideshow) {
            return {PresentationVisibility::Fullscreen, SuppressionTransitionProfile::Fast};
        }
        if (coverage.coverage == MonitorCoverageObservation::Unknown) {
            return {};
        }
        return {
            PresentationVisibility::Clear,
            state == QUNS_ACCEPTS_NOTIFICATIONS &&
                    coverage.coverage == MonitorCoverageObservation::Clear &&
                    IsPowerPointEditorMainWindow(foreground)
                ? SuppressionTransitionProfile::Fast
                : SuppressionTransitionProfile::Default,
        };
    }

    const ForegroundCoverageObservation coverage =
        ObserveForegroundMonitorCoverage(foreground);
    if (coverage.coverage == MonitorCoverageObservation::Unknown) {
        return {};
    }
    return {
        coverage.coverage == MonitorCoverageObservation::Covers
            ? PresentationVisibility::Fullscreen
            : PresentationVisibility::Clear,
        coverage.power_point_slideshow
            ? SuppressionTransitionProfile::Fast
            : SuppressionTransitionProfile::Default,
    };
}

const wchar_t* NotificationStateName(QUERY_USER_NOTIFICATION_STATE state) {
    switch (state) {
    case QUNS_NOT_PRESENT:
        return L"not_present";
    case QUNS_BUSY:
        return L"busy";
    case QUNS_RUNNING_D3D_FULL_SCREEN:
        return L"d3d_fullscreen";
    case QUNS_PRESENTATION_MODE:
        return L"presentation_mode";
    case QUNS_ACCEPTS_NOTIFICATIONS:
        return L"accepts_notifications";
    case QUNS_QUIET_TIME:
        return L"quiet_time";
    default:
        return L"unknown";
    }
}

SuppressionObservation ObserveSuppression(
    HWND foreground,
    bool screenshot_foreground,
    SuppressionReason committed_suppression) {
    const TaskbarVisibility taskbar = ObserveTaskbarVisibility();
    const PresentationObservation presentation =
        screenshot_foreground
            ? PresentationObservation{PresentationVisibility::Clear}
            : ObservePresentation(foreground, committed_suppression);
    SuppressionObservation observation = ResolveSuppressionObservation(
        taskbar,
        screenshot_foreground,
        presentation.visibility);
    if (observation.known && taskbar == TaskbarVisibility::Visible) {
        observation.transition_profile = presentation.transition_profile;
    }
    return observation;
}

const wchar_t* SuppressionReasonName(SuppressionReason reason) {
    switch (reason) {
    case SuppressionReason::TaskbarHidden:
        return L"taskbar_hidden";
    case SuppressionReason::FullscreenPresentation:
        return L"fullscreen_presentation";
    case SuppressionReason::None:
    default:
        return L"none";
    }
}

const wchar_t* SuppressionTransitionProfileName(SuppressionTransitionProfile profile) {
    return profile == SuppressionTransitionProfile::Fast ? L"fast" : L"default";
}

void LogSuppressionContext(SuppressionReason reason) {
    if (!simple_monitor::IsLogEnabled(LogLevel::Info)) {
        return;
    }

    QUERY_USER_NOTIFICATION_STATE notification_state = QUNS_NOT_PRESENT;
    const HRESULT notification_result = SHQueryUserNotificationState(&notification_state);
    HWND foreground = GetForegroundWindow();
    HWND root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
    HWND taskbar = TaskbarWindow();

    RECT foreground_rect{};
    RECT root_rect{};
    RECT monitor_rect{};
    RECT taskbar_rect{};
    GetWindowRect(foreground, &foreground_rect);
    GetWindowRect(root, &root_rect);
    GetWindowRect(taskbar, &taskbar_rect);

    HMONITOR monitor = root ? MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (monitor) {
        MONITORINFO monitor_info{};
        monitor_info.cbSize = sizeof(monitor_info);
        if (GetMonitorInfoW(monitor, &monitor_info)) {
            monitor_rect = monitor_info.rcMonitor;
        }
    }

    const std::wstring foreground_exe = WindowProcessBasename(foreground);
    const std::wstring root_exe = WindowProcessBasename(root);
    const std::wstring foreground_class = WindowClassName(foreground);
    const std::wstring root_class = WindowClassName(root);
    const std::wstring foreground_title = WindowTitleForLog(foreground);
    const std::wstring root_title = WindowTitleForLog(root);
    const LONG_PTR foreground_style = GetWindowLongPtrW(foreground, GWL_STYLE);
    const LONG_PTR foreground_ex_style = GetWindowLongPtrW(foreground, GWL_EXSTYLE);
    HWND foreground_owner = foreground ? GetWindow(foreground, GW_OWNER) : nullptr;
    HWND foreground_root_owner = foreground ? GetAncestor(foreground, GA_ROOTOWNER) : nullptr;
    LogInfo(
        L"event=suppression_context reason=%ls notification_hr=0x%08lx notification_state=%d notification_name=%ls "
        L"foreground_hwnd=%p foreground_pid=%lu foreground_exe=%ls foreground_class=%ls foreground_title=\"%ls\" "
        L"foreground_visible=%d foreground_iconic=%d foreground_style=0x%llx foreground_exstyle=0x%llx "
        L"foreground_owner=%p foreground_root_owner=%p foreground_rect=(%ld,%ld,%ld,%ld) "
        L"root_hwnd=%p root_pid=%lu root_exe=%ls root_class=%ls root_title=\"%ls\" root_rect=(%ld,%ld,%ld,%ld) "
        L"monitor_rect=(%ld,%ld,%ld,%ld) taskbar_hwnd=%p taskbar_rect=(%ld,%ld,%ld,%ld)",
        SuppressionReasonName(reason),
        static_cast<unsigned long>(notification_result),
        static_cast<int>(notification_state),
        NotificationStateName(notification_state),
        foreground,
        WindowProcessId(foreground),
        foreground_exe.c_str(),
        foreground_class.c_str(),
        foreground_title.c_str(),
        foreground && IsWindowVisible(foreground) ? 1 : 0,
        foreground && IsIconic(foreground) ? 1 : 0,
        static_cast<unsigned long long>(foreground_style),
        static_cast<unsigned long long>(foreground_ex_style),
        foreground_owner,
        foreground_root_owner,
        foreground_rect.left,
        foreground_rect.top,
        foreground_rect.right,
        foreground_rect.bottom,
        root,
        WindowProcessId(root),
        root_exe.c_str(),
        root_class.c_str(),
        root_title.c_str(),
        root_rect.left,
        root_rect.top,
        root_rect.right,
        root_rect.bottom,
        monitor_rect.left,
        monitor_rect.top,
        monitor_rect.right,
        monitor_rect.bottom,
        taskbar,
        taskbar_rect.left,
        taskbar_rect.top,
        taskbar_rect.right,
        taskbar_rect.bottom);
}

// Metric formatting and sampling.
std::wstring FormatRate(double bytes_per_second) {
    wchar_t buffer[32]{};
    double value = bytes_per_second / 1024.0;
    const wchar_t* unit = L"KB/s";
    if (value >= 1024.0) {
        value /= 1024.0;
        unit = L"MB/s";
    }

    if (value < 10.0) {
        std::swprintf(buffer, 32, L"%.1f%ls", value, unit);
    } else {
        std::swprintf(buffer, 32, L"%.0f%ls", value, unit);
    }
    return buffer;
}

std::wstring FormatPercent(double value) {
    if (value < 0.0) {
        return L"--";
    }

    wchar_t buffer[16]{};
    std::swprintf(buffer, 16, L"%.0f%%", ClampPercent(value));
    return buffer;
}

void SampleCpu(CpuSampler& sampler, Metrics& metrics) {
    FILETIME idle_ft{}, kernel_ft{}, user_ft{};
    if (!GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) {
        LogWarningRateLimited(
            L"metric.cpu.sample",
            60000,
            L"event=metric_provider_unavailable provider=cpu stage=get_system_times error=%lu",
            GetLastError());
        return;
    }
    LogFailureRecovered(
        L"metric.cpu.sample",
        L"event=component_recovered component=metric_provider provider=cpu");

    const ULONGLONG idle = FileTimeToU64(idle_ft);
    const ULONGLONG kernel = FileTimeToU64(kernel_ft);
    const ULONGLONG user = FileTimeToU64(user_ft);

    if (sampler.has_sample) {
        const ULONGLONG idle_delta = idle - sampler.idle;
        const ULONGLONG kernel_delta = kernel - sampler.kernel;
        const ULONGLONG user_delta = user - sampler.user;
        const ULONGLONG total = kernel_delta + user_delta;
        if (total > 0) {
            sampler.percent = ClampPercent((1.0 - static_cast<double>(idle_delta) / total) * 100.0);
        }
    }

    sampler.idle = idle;
    sampler.kernel = kernel;
    sampler.user = user;
    sampler.has_sample = true;
    metrics.cpu = sampler.percent;
}

void SampleMemory(Metrics& metrics) {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        metrics.memory_load = status.dwMemoryLoad;
        LogFailureRecovered(
            L"metric.memory.sample",
            L"event=component_recovered component=metric_provider provider=memory");
    } else {
        LogWarningRateLimited(
            L"metric.memory.sample",
            60000,
            L"event=metric_provider_unavailable provider=memory stage=global_memory_status error=%lu",
            GetLastError());
    }
}

NetworkSampler::InterfaceSample* FindNetworkInterfaceSample(NetworkSampler& sampler, uint64_t luid) {
    for (NetworkSampler::InterfaceSample& sample : sampler.interfaces) {
        if (sample.luid == luid) {
            return &sample;
        }
    }
    return nullptr;
}

void SampleNetwork(NetworkSampler& sampler, Metrics& metrics) {
    PMIB_IF_TABLE2 table = nullptr;
    const DWORD status = GetIfTable2(&table);
    if (status != NO_ERROR || table == nullptr) {
        sampler.availability_known = true;
        sampler.available = false;
        LogWarningRateLimited(
            L"metric.network.enumerate",
            60000,
            L"event=metric_provider_unavailable provider=network stage=get_if_table status=%lu table_null=%d",
            status,
            table == nullptr ? 1 : 0);
        return;
    }
    const bool availability_changed = !sampler.availability_known || !sampler.available;
    sampler.availability_known = true;
    sampler.available = true;

    const DWORD now = GetTickCount();
    const DWORD elapsed_ms = sampler.has_sample ? now - sampler.tick : 0;
    const double seconds = elapsed_ms > 0 ? elapsed_ms / 1000.0 : 0.0;
    double down_bps = 0.0;
    double up_bps = 0.0;
    ULONG active_interfaces = 0;

    for (NetworkSampler::InterfaceSample& sample : sampler.interfaces) {
        sample.seen = false;
    }

    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (row.OperStatus != IfOperStatusUp || row.Type == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        ++active_interfaces;

        NetworkSampler::InterfaceSample* sample = FindNetworkInterfaceSample(sampler, row.InterfaceLuid.Value);
        if (!sample) {
            sampler.interfaces.push_back({row.InterfaceLuid.Value, row.InOctets, row.OutOctets, true});
            continue;
        }

        sample->seen = true;
        if (sampler.has_sample && seconds > 0.0 &&
            row.InOctets >= sample->in_bytes &&
            row.OutOctets >= sample->out_bytes) {
            down_bps += (row.InOctets - sample->in_bytes) / seconds;
            up_bps += (row.OutOctets - sample->out_bytes) / seconds;
        }

        sample->in_bytes = row.InOctets;
        sample->out_bytes = row.OutOctets;
    }
    FreeMibTable(table);

    sampler.interfaces.erase(
        std::remove_if(
            sampler.interfaces.begin(),
            sampler.interfaces.end(),
            [](const NetworkSampler::InterfaceSample& sample) {
                return !sample.seen;
            }),
        sampler.interfaces.end());

    sampler.tick = now;
    sampler.has_sample = true;
    sampler.down_bps = down_bps;
    sampler.up_bps = up_bps;
    metrics.down_bps = down_bps;
    metrics.up_bps = up_bps;
    if (availability_changed) {
        LogInfo(
            L"event=metric_provider_available provider=network active_interfaces=%lu",
            active_interfaces);
    }
    LogFailureRecovered(
        L"metric.network.enumerate",
        L"event=component_recovered component=metric_provider provider=network active_interfaces=%lu",
        active_interfaces);
}

void ResetPdhGroup(PdhGroup& group) {
    if (group.query) {
        PdhCloseQuery(group.query);
        group.query = nullptr;
    }
    group.counter = nullptr;
    group.formatted_buffer.clear();
    group.ready = false;
    group.needs_second_sample = false;
    group.instance_count_known = false;
    group.instance_inventory_logged = false;
    group.last_instance_count = 0;
    group.last_init_attempt_tick = 0;
}

std::wstring PdhFailureKey(const PdhGroup& group, const wchar_t* stage) {
    return L"metric.pdh." + group.name + L"." + stage;
}

void ReportPdhFailure(PdhGroup& group, const wchar_t* stage, PDH_STATUS status) {
    group.availability_known = true;
    group.provider_available = false;
    group.ready = false;
    const DWORD failure_tick = GetTickCount();
    group.last_init_attempt_tick = failure_tick == 0 ? 1 : failure_tick;
    const std::wstring key = PdhFailureKey(group, stage);
    LogWarningRateLimited(
        key.c_str(),
        60000,
        L"event=metric_provider_unavailable provider=%ls stage=%ls status=0x%08lx",
        group.name.c_str(),
        stage,
        static_cast<unsigned long>(status));
}

void ReportPdhStageRecovered(PdhGroup& group, const wchar_t* stage) {
    const std::wstring key = PdhFailureKey(group, stage);
    LogFailureRecovered(
        key.c_str(),
        L"event=component_recovered component=metric_provider provider=%ls stage=%ls",
        group.name.c_str(),
        stage);
}

void ReportPdhAvailable(PdhGroup& group) {
    const bool availability_changed = !group.availability_known || !group.provider_available;
    group.availability_known = true;
    group.provider_available = true;
    if (availability_changed) {
        LogInfo(
            L"event=metric_provider_available provider=%ls mode=%ls counter_handles=%d",
            group.name.c_str(),
            group.wildcard_array ? L"wildcard_array" : L"scalar",
            group.counter ? 1 : 0);
    }
}

void InitPdhGroup(
    PdhGroup& group,
    const wchar_t* name,
    const wchar_t* wildcard_path,
    bool wildcard_array) {
    group.name = name;
    group.wildcard_path = wildcard_path;
    group.wildcard_array = wildcard_array;
    ResetPdhGroup(group);
    const DWORD init_tick = GetTickCount();
    group.last_init_attempt_tick = init_tick == 0 ? 1 : init_tick;
    const PDH_STATUS open_result = PdhOpenQueryW(nullptr, 0, &group.query);
    if (open_result != ERROR_SUCCESS) {
        ReportPdhFailure(group, L"open_query", open_result);
        return;
    }
    ReportPdhStageRecovered(group, L"open_query");

    const PDH_STATUS add_result = PdhAddCounterW(
        group.query,
        wildcard_path,
        0,
        &group.counter);
    if (add_result != ERROR_SUCCESS) {
        ReportPdhFailure(group, L"add_counter", add_result);
        PdhCloseQuery(group.query);
        group.query = nullptr;
        group.counter = nullptr;
        return;
    }
    ReportPdhStageRecovered(group, L"add_counter");

    const PDH_STATUS collect_result = PdhCollectQueryData(group.query);
    if (collect_result != ERROR_SUCCESS) {
        ReportPdhFailure(group, L"initial_collect", collect_result);
        PdhCloseQuery(group.query);
        group.query = nullptr;
        group.counter = nullptr;
        return;
    }
    ReportPdhStageRecovered(group, L"initial_collect");
    group.ready = true;
    group.needs_second_sample = true;
    ReportPdhAvailable(group);
}

void InitializePdhGroupMeasured(
    PdhGroup& group,
    const wchar_t* name,
    const wchar_t* wildcard_path,
    bool wildcard_array,
    const wchar_t* reason) {
    const ULONGLONG started = GetTickCount64();
    InitPdhGroup(group, name, wildcard_path, wildcard_array);
    group.last_init_duration_ms = GetTickCount64() - started;
    if (group.ready) {
        LogInfo(
            L"event=metric_provider_initialized provider=%ls reason=%ls mode=%ls "
            L"counter_handles=%d duration_ms=%llu",
            group.name.c_str(),
            reason,
            group.wildcard_array ? L"wildcard_array" : L"scalar",
            group.counter ? 1 : 0,
            static_cast<unsigned long long>(group.last_init_duration_ms));
    }
}

void RebuildPdhGroup(PdhGroup& group) {
    if (group.wildcard_path.empty()) {
        return;
    }

    const std::wstring name = group.name;
    const std::wstring wildcard_path = group.wildcard_path;
    const bool wildcard_array = group.wildcard_array;
    InitializePdhGroupMeasured(
        group,
        name.c_str(),
        wildcard_path.c_str(),
        wildcard_array,
        L"retry");
}

void RecoverPdhGroupIfNeeded(PdhGroup& group) {
    if (group.wildcard_path.empty()) {
        return;
    }

    const DWORD now = GetTickCount();
    if (ShouldReinitializePdhGroup(
            group.ready,
            group.query != nullptr && group.counter != nullptr,
            now,
            group.last_init_attempt_tick,
            kPdhRetryIntervalMs)) {
        RebuildPdhGroup(group);
    }
}

bool IsPdhValueStatusValid(PDH_STATUS status) {
    return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA;
}

bool IsTransientPdhCalculationStatus(PDH_STATUS status) {
    return status == static_cast<PDH_STATUS>(PDH_CALC_NEGATIVE_DENOMINATOR) ||
           status == static_cast<PDH_STATUS>(PDH_CALC_NEGATIVE_TIMEBASE) ||
           status == static_cast<PDH_STATUS>(PDH_CALC_NEGATIVE_VALUE);
}

void ReportPdhSampleTransient(PdhGroup& group, PDH_STATUS status) {
    const std::wstring key = PdhFailureKey(group, L"sample_transient");
    LogWarningRateLimited(
        key.c_str(),
        60000,
        L"event=metric_sample_held provider=%ls stage=format_value status=0x%08lx",
        group.name.c_str(),
        static_cast<unsigned long>(status));
}

void ReportPdhSampleRecovered(PdhGroup& group) {
    const std::wstring key = PdhFailureKey(group, L"sample_transient");
    LogFailureRecovered(
        key.c_str(),
        L"event=component_recovered component=metric_sample provider=%ls stage=format_value",
        group.name.c_str());
}

PDH_STATUS ReadFormattedCounterArray(PdhGroup& group, DWORD& item_count) {
    item_count = 0;
    if (!group.formatted_buffer.empty()) {
        DWORD buffer_size = static_cast<DWORD>(group.formatted_buffer.size());
        const PDH_STATUS existing_buffer_result = PdhGetFormattedCounterArrayW(
            group.counter,
            PDH_FMT_DOUBLE,
            &buffer_size,
            &item_count,
            reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(group.formatted_buffer.data()));
        if (existing_buffer_result == ERROR_SUCCESS) {
            return ERROR_SUCCESS;
        }
        if (existing_buffer_result != static_cast<PDH_STATUS>(PDH_MORE_DATA)) {
            return existing_buffer_result;
        }
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        DWORD buffer_size = 0;
        item_count = 0;
        const PDH_STATUS size_result = PdhGetFormattedCounterArrayW(
            group.counter,
            PDH_FMT_DOUBLE,
            &buffer_size,
            &item_count,
            nullptr);
        if (size_result == ERROR_SUCCESS && buffer_size == 0 && item_count == 0) {
            group.formatted_buffer.clear();
            return ERROR_SUCCESS;
        }
        if (size_result != static_cast<PDH_STATUS>(PDH_MORE_DATA) || buffer_size == 0) {
            return size_result;
        }

        group.formatted_buffer.resize(buffer_size);
        DWORD available_size = static_cast<DWORD>(group.formatted_buffer.size());
        const PDH_STATUS values_result = PdhGetFormattedCounterArrayW(
            group.counter,
            PDH_FMT_DOUBLE,
            &available_size,
            &item_count,
            reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(group.formatted_buffer.data()));
        if (values_result == ERROR_SUCCESS) {
            return ERROR_SUCCESS;
        }
        if (values_result != static_cast<PDH_STATUS>(PDH_MORE_DATA)) {
            return values_result;
        }
    }

    return PDH_MORE_DATA;
}

double SamplePdhGroup(PdhGroup& group) {
    if (!group.ready || group.query == nullptr || group.counter == nullptr) {
        return -1.0;
    }

    const PDH_STATUS collect_result = PdhCollectQueryData(group.query);
    if (collect_result != ERROR_SUCCESS) {
        ReportPdhFailure(group, L"collect", collect_result);
        return -1.0;
    }
    ReportPdhStageRecovered(group, L"collect");

    if (group.needs_second_sample) {
        group.needs_second_sample = false;
        return group.value;
    }

    double max_value = -1.0;
    bool any = false;
    PDH_STATUS first_format_failure = ERROR_SUCCESS;

    if (group.wildcard_array) {
        DWORD item_count = 0;
        const PDH_STATUS format_result = ReadFormattedCounterArray(group, item_count);
        if (format_result != ERROR_SUCCESS) {
            ReportPdhFailure(group, L"format_array", format_result);
            return -1.0;
        }
        ReportPdhStageRecovered(group, L"format_array");

        group.instance_count_known = true;
        group.last_instance_count = item_count;
        if (!group.instance_inventory_logged) {
            group.instance_inventory_logged = true;
            LogInfo(
                L"event=metric_provider_inventory provider=%ls instances=%lu",
                group.name.c_str(),
                item_count);
        }

        const auto* items = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(
            group.formatted_buffer.data());
        std::vector<GpuEngineSampleView> samples;
        samples.reserve(item_count);
        for (DWORD index = 0; index < item_count; ++index) {
            const PDH_FMT_COUNTERVALUE& value = items[index].FmtValue;
            if (!IsPdhValueStatusValid(value.CStatus)) {
                if (first_format_failure == ERROR_SUCCESS) {
                    first_format_failure = value.CStatus;
                }
                continue;
            }

            any = true;
            samples.push_back({items[index].szName, value.doubleValue});
        }
        if (item_count == 0) {
            any = true;
            max_value = 0.0;
        } else if (any) {
            max_value = AggregateBusiestGpuEngine(samples);
        }
    } else {
        PDH_FMT_COUNTERVALUE value{};
        const PDH_STATUS format_result = PdhGetFormattedCounterValue(
            group.counter,
            PDH_FMT_DOUBLE,
            nullptr,
            &value);
        if (format_result == ERROR_SUCCESS && IsPdhValueStatusValid(value.CStatus)) {
            any = true;
            max_value = value.doubleValue;
        } else {
            first_format_failure = format_result != ERROR_SUCCESS ? format_result : value.CStatus;
        }
    }

    if (!any) {
        if (ClassifyPdhSample(
                false,
                IsTransientPdhCalculationStatus(first_format_failure)) ==
            PdhSampleDisposition::KeepPrevious) {
            ReportPdhSampleTransient(group, first_format_failure);
            return group.value;
        }
        ReportPdhFailure(group, L"format_value", first_format_failure);
        return -1.0;
    }
    ReportPdhStageRecovered(group, L"format_value");
    ReportPdhSampleRecovered(group);
    ReportPdhAvailable(group);

    group.value = ClampPercent(max_value);
    return group.value;
}

void SampleKeys(Metrics& metrics) {
    metrics.caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
    metrics.insert = (GetKeyState(VK_INSERT) & 1) != 0;
    metrics.num = (GetKeyState(VK_NUMLOCK) & 1) != 0;
}

bool SampleKeysIfChanged() {
    Metrics next = g_app.metrics.current;
    SampleKeys(next);
    const bool changed =
        next.caps != g_app.metrics.current.caps ||
        next.insert != g_app.metrics.current.insert ||
        next.num != g_app.metrics.current.num;
    if (changed) {
        g_app.metrics.current.caps = next.caps;
        g_app.metrics.current.insert = next.insert;
        g_app.metrics.current.num = next.num;
    }
    return changed;
}

void SampleMetrics() {
    SampleCpu(g_app.metrics.cpu, g_app.metrics.current);
    SampleMemory(g_app.metrics.current);
    SampleNetwork(g_app.metrics.network, g_app.metrics.current);
    RecoverPdhGroupIfNeeded(g_app.metrics.gpu);
    RecoverPdhGroupIfNeeded(g_app.metrics.disk);
    g_app.metrics.current.gpu = SamplePdhGroup(g_app.metrics.gpu);
    g_app.metrics.current.disk = SamplePdhGroup(g_app.metrics.disk);
    SampleKeys(g_app.metrics.current);
    g_app.diagnostics.last_metrics_sample_tick = GetTickCount();
}

// Overlay style and taskbar layout events.
bool UpdateLayeredStyle(HWND hwnd) {
    LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex_style |= WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE;
    if (g_app.tray.click_through) {
        ex_style |= WS_EX_TRANSPARENT;
    } else {
        ex_style &= ~WS_EX_TRANSPARENT;
    }
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous_style = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style);
    const DWORD style_error = previous_style == 0 ? GetLastError() : ERROR_SUCCESS;
    if (style_error != ERROR_SUCCESS) {
        LogErrorRateLimited(
            L"overlay.update_style",
            kFailureLogIntervalMs,
            L"event=overlay_style_failed stage=click_through error=%lu requested=0x%llx",
            style_error,
            static_cast<unsigned long long>(ex_style));
    } else {
        LogFailureRecovered(
            L"overlay.update_style",
            L"event=component_recovered component=overlay_style stage=click_through");
    }
    return style_error == ERROR_SUCCESS;
}

void CALLBACK TrayEventProc(
    HWINEVENTHOOK,
    DWORD event,
    HWND hwnd,
    LONG id_object,
    LONG,
    DWORD,
    DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND) {
        if (g_app.window.controller_hwnd &&
            IsWindow(g_app.window.controller_hwnd) &&
            hwnd != g_app.window.overlay_hwnd) {
            if (!PostMessageW(
                    g_app.window.controller_hwnd,
                    WM_FOREGROUND_CHANGED,
                    event,
                    reinterpret_cast<LPARAM>(hwnd))) {
                LogWarningRateLimited(
                    L"shell.post_foreground",
                    kFailureLogIntervalMs,
                    L"event=shell_message_failed message=foreground error=%lu",
                    GetLastError());
            } else {
                LogFailureRecovered(
                    L"shell.post_foreground",
                    L"event=component_recovered component=shell_message message=foreground");
            }
        }
        return;
    }

    if (!g_app.window.controller_hwnd ||
        !IsWindow(g_app.window.controller_hwnd) ||
        !IsTaskbarRelatedWindow(hwnd)) {
        return;
    }

    if (id_object != OBJID_WINDOW && id_object != OBJID_CLIENT) {
        return;
    }

    if (InterlockedCompareExchange(&g_app.placement.tray_layout_update_pending, 1, 0) == 0) {
        if (!PostMessageW(g_app.window.controller_hwnd, WM_TRAY_LAYOUT_CHANGED, 0, 0)) {
            const DWORD error = GetLastError();
            InterlockedExchange(&g_app.placement.tray_layout_update_pending, 0);
            LogWarningRateLimited(
                L"shell.post_tray_layout",
                kFailureLogIntervalMs,
                L"event=shell_message_failed message=tray_layout error=%lu",
                error);
        } else {
            LogFailureRecovered(
                L"shell.post_tray_layout",
                L"event=component_recovered component=shell_message message=tray_layout");
        }
    }
}

void RegisterTrayEventHooks() {
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    const DWORD events[] = {
        EVENT_OBJECT_SHOW,
        EVENT_OBJECT_HIDE,
        EVENT_OBJECT_REORDER,
        EVENT_OBJECT_LOCATIONCHANGE,
        EVENT_SYSTEM_FOREGROUND,
    };

    size_t installed = 0;
    for (DWORD event : events) {
        if (HWINEVENTHOOK hook = SetWinEventHook(event, event, nullptr, TrayEventProc, 0, 0, flags)) {
            g_app.tray.event_hooks.push_back(hook);
            ++installed;
        } else {
            LogWarning(L"event=win_event_hook_failed event_id=%lu", event);
        }
    }
    LogInfo(
        L"event=win_event_hooks_registered installed=%llu expected=%llu",
        static_cast<unsigned long long>(installed),
        static_cast<unsigned long long>(ARRAYSIZE(events)));
}

void UnregisterTrayEventHooks() {
    size_t failed = 0;
    for (HWINEVENTHOOK hook : g_app.tray.event_hooks) {
        if (!UnhookWinEvent(hook)) {
            ++failed;
        }
    }
    if (failed > 0) {
        LogWarning(
            L"event=win_event_hook_cleanup_failed failed=%llu total=%llu",
            static_cast<unsigned long long>(failed),
            static_cast<unsigned long long>(g_app.tray.event_hooks.size()));
    }
    g_app.tray.event_hooks.clear();
}

const wchar_t* TimerFailureKey(UINT_PTR timer_id) {
    switch (timer_id) {
    case kRefreshTimer:
        return L"timer_install_refresh";
    case kPlacementTimer:
        return L"timer_install_placement";
    case kStateTimer:
        return L"timer_install_state";
    case kStartupInitTimer:
        return L"timer_install_startup_init";
    default:
        return L"timer_install_other";
    }
}

bool InstallTimer(
    HWND hwnd,
    UINT_PTR timer_id,
    UINT interval_ms,
    const wchar_t* timer_name) {
    const wchar_t* failure_key = TimerFailureKey(timer_id);
    if (!hwnd) {
        LogErrorRateLimited(
            failure_key,
            kFailureLogIntervalMs,
            L"event=timer_install_failed timer=%ls id=%llu interval_ms=%u error_available=1 error=%lu",
            timer_name,
            static_cast<unsigned long long>(timer_id),
            interval_ms,
            static_cast<unsigned long>(ERROR_INVALID_WINDOW_HANDLE));
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    if (SetTimer(hwnd, timer_id, interval_ms, nullptr) == 0) {
        const DWORD error = GetLastError();
        LogErrorRateLimited(
            failure_key,
            kFailureLogIntervalMs,
            L"event=timer_install_failed timer=%ls id=%llu interval_ms=%u error_available=%d error=%lu",
            timer_name,
            static_cast<unsigned long long>(timer_id),
            interval_ms,
            error != ERROR_SUCCESS ? 1 : 0,
            error);
        return false;
    }
    LogFailureRecovered(
        failure_key,
        L"event=timer_install_recovered timer=%ls id=%llu interval_ms=%u",
        timer_name,
        static_cast<unsigned long long>(timer_id),
        interval_ms);
    return true;
}

bool SetPlacementTimer(UINT interval_ms) {
    return g_app.window.controller_hwnd &&
           InstallTimer(
               g_app.window.controller_hwnd,
               kPlacementTimer,
               interval_ms,
               L"placement");
}

// Taskbar anchoring and placement.
HWND FindDescendantWindow(HWND parent, const wchar_t* class_name) {
    HWND child = nullptr;
    while ((child = FindWindowExW(parent, child, nullptr, nullptr)) != nullptr) {
        wchar_t current_class[128]{};
        GetClassNameW(child, current_class, 128);
        if (std::wcscmp(current_class, class_name) == 0) {
            return child;
        }
        if (HWND nested = FindDescendantWindow(child, class_name)) {
            return nested;
        }
    }
    return nullptr;
}

HWND TrayNotifyWindow() {
    HWND tray = TaskbarWindow();
    return tray ? FindDescendantWindow(tray, L"TrayNotifyWnd") : nullptr;
}

bool TryGetTrayNotifyRect(RECT& rect) {
    HWND notify = TrayNotifyWindow();
    if (!notify) {
        return false;
    }
    return GetWindowRect(notify, &rect) != 0;
}

bool IsAccessibleVisible(IAccessible* accessible, VARIANT child_id) {
    VARIANT state{};
    VariantInit(&state);
    const HRESULT hr = accessible->get_accState(child_id, &state);
    const bool visible =
        SUCCEEDED(hr) &&
        state.vt == VT_I4 &&
        (state.lVal & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN | STATE_SYSTEM_UNAVAILABLE)) == 0;
    VariantClear(&state);
    return visible;
}

bool TryGetAccessibleRect(IAccessible* accessible, VARIANT child_id, RECT& rect) {
    LONG left = 0;
    LONG top = 0;
    LONG width = 0;
    LONG height = 0;
    if (FAILED(accessible->accLocation(&left, &top, &width, &height, child_id)) || width <= 0 || height <= 0) {
        return false;
    }

    rect = {left, top, left + width, top + height};
    return true;
}

bool IsUsefulAnchorRect(const RECT& rect, const RECT& tray_rect) {
    RECT overlap{};
    if (!IntersectRect(&overlap, &rect, &tray_rect)) {
        return false;
    }

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int tray_width = tray_rect.right - tray_rect.left;
    const int tray_height = tray_rect.bottom - tray_rect.top;
    if (width < 4 || height < 4) {
        return false;
    }

    if (width >= tray_width - 4 && height >= tray_height - 4) {
        return false;
    }

    return true;
}

void ConsiderAnchorRect(const RECT& candidate, RECT& best_rect, bool& found) {
    if (!found ||
        candidate.left < best_rect.left ||
        (candidate.left == best_rect.left && candidate.top < best_rect.top) ||
        (candidate.left == best_rect.left && candidate.top == best_rect.top &&
        (candidate.right - candidate.left) > (best_rect.right - best_rect.left))) {
        best_rect = candidate;
        found = true;
    }
}

void CollectAccessibleAnchorRects(
    IAccessible* accessible,
    const RECT& tray_rect,
    int depth,
    bool include_self,
    RECT& best_rect,
    bool& found) {
    VARIANT self_id{};
    VariantInit(&self_id);
    self_id.vt = VT_I4;
    self_id.lVal = CHILDID_SELF;

    if (include_self && IsAccessibleVisible(accessible, self_id)) {
        RECT rect{};
        if (TryGetAccessibleRect(accessible, self_id, rect) && IsUsefulAnchorRect(rect, tray_rect)) {
            ConsiderAnchorRect(rect, best_rect, found);
        }
    }

    if (depth <= 0) {
        return;
    }

    LONG child_count = 0;
    if (FAILED(accessible->get_accChildCount(&child_count)) || child_count <= 0) {
        return;
    }

    std::vector<VARIANT> children(static_cast<size_t>(child_count));
    for (VARIANT& child : children) {
        VariantInit(&child);
    }

    LONG obtained = 0;
    if (FAILED(AccessibleChildren(accessible, 0, child_count, children.data(), &obtained))) {
        for (VARIANT& child : children) {
            VariantClear(&child);
        }
        return;
    }

    for (LONG i = 0; i < obtained; ++i) {
        VARIANT& child = children[static_cast<size_t>(i)];
        if (child.vt == VT_I4) {
            if (IsAccessibleVisible(accessible, child)) {
                RECT rect{};
                if (TryGetAccessibleRect(accessible, child, rect) && IsUsefulAnchorRect(rect, tray_rect)) {
                    ConsiderAnchorRect(rect, best_rect, found);
                }
            }
        } else if (child.vt == VT_DISPATCH && child.pdispVal) {
            IAccessible* child_accessible = nullptr;
            if (SUCCEEDED(child.pdispVal->QueryInterface(IID_IAccessible, reinterpret_cast<void**>(&child_accessible))) &&
                child_accessible) {
                CollectAccessibleAnchorRects(child_accessible, tray_rect, depth - 1, true, best_rect, found);
                child_accessible->Release();
            }
        }
        VariantClear(&child);
    }
}

bool TryGetTrayAnchorRect(RECT& rect) {
    RECT tray_rect{};
    if (!TryGetTrayNotifyRect(tray_rect)) {
        return false;
    }

    HWND notify = TrayNotifyWindow();
    if (!notify) {
        return false;
    }

    IAccessible* accessible = nullptr;
    HRESULT hr = AccessibleObjectFromWindow(
        notify,
        OBJID_CLIENT,
        IID_IAccessible,
        reinterpret_cast<void**>(&accessible));
    if (FAILED(hr) || !accessible) {
        return false;
    }

    RECT best_rect{};
    bool found = false;
    CollectAccessibleAnchorRects(accessible, tray_rect, 2, false, best_rect, found);
    accessible->Release();

    if (!found) {
        return false;
    }

    rect = best_rect;
    return true;
}

int EstimatedTextWidthDip(int chars, int font_size_dip = 0) {
    const int font_size = font_size_dip > 0 ? font_size_dip : g_app.config.font_size_dip;
    return chars * font_size * 6 / 10 + 2;
}

int CalculateOverlayWidthDip() {
    const int network_width =
        EstimatedTextWidthDip(1, g_app.config.network_arrow_font_size_dip) +
        EstimatedTextWidthDip(1) +
        g_app.config.network_arrow_gap_dip +
        EstimatedTextWidthDip(11); // Reserve enough width for the largest network rate label.
    const int system_width = EstimatedTextWidthDip(9);  // Covers strings like "RAM: 100%".
    const int disk_label_chars = 3;
    const int disk_width = EstimatedTextWidthDip(std::max(9, disk_label_chars + 6));
    const int key_width = g_app.config.show_key_widget ? EstimatedTextWidthDip(11, g_app.config.key_font_size_dip) : 0;

    const int gap_network =
        g_app.config.gap_after_network_dip >= 0 ? g_app.config.gap_after_network_dip : g_app.config.column_gap_dip;
    const int gap_system =
        g_app.config.gap_after_system_dip >= 0 ? g_app.config.gap_after_system_dip : g_app.config.column_gap_dip;

    const int content_width =
        g_app.config.content_padding_x_dip * 2 +
        network_width +
        gap_network +
        system_width +
        gap_system +
        disk_width +
        (g_app.config.show_key_widget ? g_app.config.gap_after_disk_dip + key_width : 0);

    return content_width;
}

bool GetTaskbarRect(RECT& rect) {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &abd) == 0) {
        return false;
    }
    rect = abd.rc;
    return true;
}

bool RepositionWindow() {
    HWND overlay = g_app.window.overlay_hwnd;
    if (!overlay || !IsWindow(overlay)) {
        return false;
    }

    RECT taskbar{};
    if (!GetTaskbarRect(taskbar)) {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        taskbar = {work.left, work.bottom - Scale(48, g_app.window.dpi), work.right, work.bottom};
    }

    g_app.window.dpi = WindowDpi(overlay);

    const int width = Scale(CalculateOverlayWidthDip(), g_app.window.dpi);
    const int min_height = Scale(36, g_app.window.dpi);
    const int taskbar_width = taskbar.right - taskbar.left;
    const int taskbar_height = taskbar.bottom - taskbar.top;
    const bool horizontal = taskbar_width >= taskbar_height;
    const int height = horizontal ? std::max(min_height, taskbar_height) : Scale(132, g_app.window.dpi);

    int x = taskbar.left;
    int y = taskbar.top;
    int anchor_mode = 0;
    RECT anchor_rect{};

    if (horizontal) {
        int tray_left = taskbar.right - Scale(360, g_app.window.dpi);
        if (TryGetTrayAnchorRect(anchor_rect)) {
            tray_left = anchor_rect.left;
            anchor_mode = 1;
        } else {
            RECT notify_rect{};
            if (TryGetTrayNotifyRect(notify_rect)) {
                tray_left = notify_rect.left;
                anchor_rect = notify_rect;
                anchor_mode = 2;
            }
        }

        x = std::max(static_cast<int>(taskbar.left), tray_left - width - Scale(g_app.config.offset_right_dip, g_app.window.dpi));
        y = taskbar.top + std::max(0, (taskbar_height - height) / 2);
    } else if (taskbar.left <= 0) {
        x = taskbar.left;
        y = taskbar.bottom - height - Scale(8, g_app.window.dpi);
    } else {
        x = taskbar.right - width;
        y = taskbar.bottom - height - Scale(8, g_app.window.dpi);
    }

    const RECT requested_rect{x, y, x + width, y + height};
    RECT previous_rect{};
    const bool previous_known = GetWindowRect(overlay, &previous_rect) != FALSE;
    const bool geometry_changed = !previous_known || !RectEquals(previous_rect, requested_rect);
    const bool needs_topmost =
        (GetWindowLongPtrW(overlay, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0;

    BOOL positioned = TRUE;
    if (geometry_changed || needs_topmost) {
        positioned = SetWindowPos(
            overlay,
            HWND_TOPMOST,
            x,
            y,
            width,
            height,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    const DWORD position_error = positioned ? ERROR_SUCCESS : GetLastError();
    if (positioned) {
        g_app.diagnostics.last_reposition_success_tick = GetTickCount();
        LogFailureRecovered(
            L"reposition.set_window_pos",
            L"event=component_recovered component=reposition stage=set_window_pos");
    } else {
        LogErrorRateLimited(
            L"reposition.set_window_pos",
            kFailureLogIntervalMs,
            L"event=placement_failed stage=set_window_pos error=%lu requested=(%d,%d,%d,%d)",
            position_error,
            x,
            y,
            x + width,
            y + height);
    }

    RECT overlay_rect{};
    bool actual_known = false;
    if (positioned) {
        actual_known = GetWindowRect(overlay, &overlay_rect) != FALSE;
        if (!actual_known) {
            LogWarningRateLimited(
                L"reposition.get_window_rect",
                kFailureLogIntervalMs,
                L"event=placement_probe_failed stage=get_window_rect error=%lu",
                GetLastError());
        } else {
            LogFailureRecovered(
                L"reposition.get_window_rect",
                L"event=component_recovered component=reposition stage=get_window_rect");
        }
    }
    const bool taskbar_changed = !g_app.placement.has_last_logged_taskbar_rect || !RectEquals(taskbar, g_app.placement.last_logged_taskbar_rect);
    const bool anchor_changed =
        anchor_mode != g_app.placement.last_logged_anchor_mode ||
        (anchor_mode == 0 ? g_app.placement.has_last_logged_anchor_rect :
                            !g_app.placement.has_last_logged_anchor_rect || !RectEquals(anchor_rect, g_app.placement.last_logged_anchor_rect));
    const bool overlay_changed =
        actual_known &&
        (!g_app.placement.has_last_logged_overlay_rect ||
         !RectEquals(overlay_rect, g_app.placement.last_logged_overlay_rect));
    if (actual_known && (taskbar_changed || anchor_changed || overlay_changed)) {
        LogInfo(
            L"event=placement result=ok actual_known=1 taskbar=(%ld,%ld,%ld,%ld) anchor_mode=%d "
            L"anchor=(%ld,%ld,%ld,%ld) requested=(%ld,%ld,%ld,%ld) actual=(%ld,%ld,%ld,%ld)",
            taskbar.left,
            taskbar.top,
            taskbar.right,
            taskbar.bottom,
            anchor_mode,
            anchor_rect.left,
            anchor_rect.top,
            anchor_rect.right,
            anchor_rect.bottom,
            requested_rect.left,
            requested_rect.top,
            requested_rect.right,
            requested_rect.bottom,
            overlay_rect.left,
            overlay_rect.top,
            overlay_rect.right,
            overlay_rect.bottom);
        g_app.placement.last_logged_taskbar_rect = taskbar;
        g_app.placement.last_logged_anchor_rect = anchor_rect;
        g_app.placement.last_logged_overlay_rect = overlay_rect;
        g_app.placement.has_last_logged_taskbar_rect = true;
        g_app.placement.has_last_logged_anchor_rect = anchor_mode != 0;
        g_app.placement.has_last_logged_overlay_rect = true;
        g_app.placement.last_logged_anchor_mode = anchor_mode;
    }

    return positioned && geometry_changed;
}

// Tray icon and context menu.
HICON LoadAppIcon(HINSTANCE instance, int width, int height) {
    HICON icon = reinterpret_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(kAppIconResource),
        IMAGE_ICON,
        width,
        height,
        LR_DEFAULTCOLOR | LR_SHARED));
    if (!icon) {
        icon = LoadIconW(instance, MAKEINTRESOURCEW(kAppIconResource));
    }
    if (!icon) {
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    return icon;
}

void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadAppIcon(g_app.window.instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    std::wcsncpy(nid.szTip, L"Simple Monitor", ARRAYSIZE(nid.szTip) - 1);
    const BOOL added = Shell_NotifyIconW(NIM_ADD, &nid);
    if (!added) {
        LogWarningRateLimited(
            L"tray.add",
            kFailureLogIntervalMs,
            L"event=tray_icon_failed operation=add taskbar=%p taskbar_pid=%lu",
            TaskbarWindow(),
            WindowProcessId(TaskbarWindow()));
        return;
    }
    LogFailureRecovered(
        L"tray.add",
        L"event=component_recovered component=tray_icon operation=add");

    nid.uVersion = NOTIFYICON_VERSION;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &nid)) {
        LogWarningRateLimited(
            L"tray.set_version",
            kFailureLogIntervalMs,
            L"event=tray_icon_failed operation=set_version");
    } else {
        LogFailureRecovered(
            L"tray.set_version",
            L"event=component_recovered component=tray_icon operation=set_version");
    }
}

void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;
    if (!Shell_NotifyIconW(NIM_DELETE, &nid)) {
        LogDebug(L"event=tray_icon_remove_skipped result=not_found_or_shell_unavailable");
    }
}

void LogOverlayWindowState(const wchar_t* event_name, HWND overlay) {
    if (!simple_monitor::IsLogEnabled(LogLevel::Info) || !overlay || !IsWindow(overlay)) {
        return;
    }

    const LONG_PTR ex_style = GetWindowLongPtrW(overlay, GWL_EXSTYLE);
    HWND above = GetWindow(overlay, GW_HWNDPREV);
    const std::wstring above_exe = WindowProcessBasename(above);
    const std::wstring above_class = WindowClassName(above);
    LogInfo(
        L"event=%ls hwnd=%p visible=%d topmost=%d owner=%p above_hwnd=%p above_exe=%ls above_class=%ls",
        event_name,
        overlay,
        IsWindowVisible(overlay) ? 1 : 0,
        (ex_style & WS_EX_TOPMOST) != 0 ? 1 : 0,
        GetWindow(overlay, GW_OWNER),
        above,
        above_exe.c_str(),
        above_class.c_str());
}

struct OverlayStateSnapshot {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HWND expected_owner = nullptr;
    HWND above = nullptr;
    RECT rect{};
    LONG_PTR ex_style = 0;
    unsigned issues = OverlayInvariantNone;
    bool valid = false;
    bool visible = false;
    bool cloak_known = false;
    HRESULT cloak_hresult = E_FAIL;
    DWORD cloak_flags = 0;
};

OverlayStateSnapshot CaptureOverlayState(bool expected_visible) {
    OverlayStateSnapshot state;
    state.hwnd = g_app.window.overlay_hwnd;
    state.expected_owner = CommittedTaskbarWindow();
    state.valid = state.hwnd && IsWindow(state.hwnd);
    if (!state.valid) {
        state.issues |= OverlayInvariantMissing;
        return state;
    }

    state.visible = IsWindowVisible(state.hwnd) != FALSE;
    state.ex_style = GetWindowLongPtrW(state.hwnd, GWL_EXSTYLE);
    state.owner = GetWindow(state.hwnd, GW_OWNER);
    state.above = GetWindow(state.hwnd, GW_HWNDPREV);
    state.cloak_hresult = DwmGetWindowAttribute(
        state.hwnd,
        DWMWA_CLOAKED,
        &state.cloak_flags,
        sizeof(state.cloak_flags));
    state.cloak_known = SUCCEEDED(state.cloak_hresult);
    if (!state.cloak_known) {
        state.cloak_flags = 0;
        LogWarningRateLimited(
            L"overlay.dwm_cloak_probe",
            kFailureLogIntervalMs,
            L"event=overlay_probe_failed stage=dwm_cloaked hresult=0x%08lx",
            static_cast<unsigned long>(state.cloak_hresult));
    } else {
        LogFailureRecovered(
            L"overlay.dwm_cloak_probe",
            L"event=component_recovered component=overlay_probe stage=dwm_cloaked");
    }

    if (expected_visible && !state.visible) {
        state.issues |= OverlayInvariantHidden;
    }
    if ((state.ex_style & WS_EX_TOPMOST) == 0) {
        state.issues |= OverlayInvariantNotTopmost;
    }
    // Windows Shell can temporarily clear GW_OWNER while Start or Search is
    // open. Keep the actual and expected owners in diagnostics, but do not
    // treat that transient Shell state as overlay damage. Taskbar generation
    // changes are detected separately by HWND and process ID.
    if ((state.ex_style & WS_EX_LAYERED) == 0) {
        state.issues |= OverlayInvariantMissingLayeredStyle;
    }
    if (expected_visible && state.cloak_known && state.cloak_flags != 0) {
        state.issues |= OverlayInvariantCloaked;
    }

    if (!GetWindowRect(state.hwnd, &state.rect) ||
        state.rect.right <= state.rect.left ||
        state.rect.bottom <= state.rect.top) {
        if (expected_visible) {
            state.issues |= OverlayInvariantInvalidRect;
        }
    }

    const DWORD now = GetTickCount();
    if (expected_visible && !g_app.suppression.overlay_update_frozen) {
        const bool successful_present_stale =
            g_app.diagnostics.last_present_success_tick != 0 &&
            now - g_app.diagnostics.last_present_success_tick > kPresentStaleThresholdMs;
        const bool no_successful_present =
            g_app.diagnostics.last_present_success_tick == 0 &&
            g_app.diagnostics.overlay_created_tick != 0 &&
            TickPassed(
                now,
                g_app.diagnostics.overlay_created_tick + kPresentStaleThresholdMs);
        if (successful_present_stale || no_successful_present) {
            state.issues |= OverlayInvariantPresentStale;
        }
    }
    return state;
}

void LogOverlayInvariantFailure(const wchar_t* trigger, const OverlayStateSnapshot& state) {
    HWND taskbar = state.expected_owner;
    RECT taskbar_rect{};
    GetWindowRect(taskbar, &taskbar_rect);
    const DWORD now = GetTickCount();
    const std::wstring above_exe = WindowProcessBasename(state.above);
    const std::wstring above_class = WindowClassName(state.above);
    HWND foreground = GetForegroundWindow();
    const std::wstring foreground_exe = WindowProcessBasename(foreground);
    const std::wstring foreground_class = WindowClassName(foreground);

    LogWarningRateLimited(
        L"overlay.invariant",
        kFailureLogIntervalMs,
        L"event=overlay_invariant_failed trigger=%ls issues=0x%02x hwnd=%p valid=%d visible=%d "
        L"cloak_known=%d cloak_hresult=0x%08lx cloak_flags=0x%08lx "
        L"topmost=%d layered=%d owner=%p expected_owner=%p rect=(%ld,%ld,%ld,%ld) "
        L"taskbar=%p taskbar_pid=%lu taskbar_rect=(%ld,%ld,%ld,%ld) above=%p above_exe=%ls "
        L"above_class=%ls foreground=%p foreground_exe=%ls foreground_class=%ls suppression=%ls "
        L"frozen=%d last_present_age_ms=%lld last_reposition_age_ms=%lld",
        trigger,
        state.issues,
        state.hwnd,
        state.valid ? 1 : 0,
        state.visible ? 1 : 0,
        state.cloak_known ? 1 : 0,
        static_cast<unsigned long>(state.cloak_hresult),
        static_cast<unsigned long>(state.cloak_flags),
        (state.ex_style & WS_EX_TOPMOST) != 0 ? 1 : 0,
        (state.ex_style & WS_EX_LAYERED) != 0 ? 1 : 0,
        state.owner,
        state.expected_owner,
        state.rect.left,
        state.rect.top,
        state.rect.right,
        state.rect.bottom,
        taskbar,
        WindowProcessId(taskbar),
        taskbar_rect.left,
        taskbar_rect.top,
        taskbar_rect.right,
        taskbar_rect.bottom,
        state.above,
        above_exe.c_str(),
        above_class.c_str(),
        foreground,
        foreground_exe.c_str(),
        foreground_class.c_str(),
        SuppressionReasonName(g_app.suppression.policy.committed),
        g_app.suppression.overlay_update_frozen ? 1 : 0,
        TickAgeMs(now, g_app.diagnostics.last_present_success_tick),
        TickAgeMs(now, g_app.diagnostics.last_reposition_success_tick));
}

bool ShouldLogOverlayInvariantDetail(DWORD now) {
    if (!simple_monitor::IsLogEnabled(LogLevel::Warning)) {
        return false;
    }
    if (g_app.diagnostics.last_overlay_detail_log_tick != 0 &&
        !TickPassed(
            now,
            g_app.diagnostics.last_overlay_detail_log_tick + kFailureLogIntervalMs)) {
        return false;
    }
    g_app.diagnostics.last_overlay_detail_log_tick = now == 0 ? 1 : now;
    return true;
}

void ResetInitialOwnerBindingState() {
    g_app.reconcile.initial_owner_binding = {};
}

void ArmInitialOwnerBindingVerification(
    HWND overlay,
    HWND target,
    const wchar_t* source,
    std::uint64_t delay_ms) {
    ResetInitialOwnerBindingState();
    if (!overlay || !IsWindow(overlay) || !target || !IsWindow(target)) {
        return;
    }

    const HWND effective_owner = GetWindow(overlay, GW_OWNER);
    auto& state = g_app.reconcile.initial_owner_binding;
    state.target = target;
    state.next_retry_ms = GetTickCount64() + delay_ms;
    state.pending = true;
    if (effective_owner != target) {
        LogWarning(
            L"event=overlay_owner_binding result=pending source=%ls hwnd=%p target=%p effective_owner=%p "
            L"generation=%llu retry_delay_ms=%llu max_attempts=%u",
            source,
            overlay,
            target,
            effective_owner,
            static_cast<unsigned long long>(g_app.diagnostics.overlay_generation),
            static_cast<unsigned long long>(delay_ms),
            kInitialOwnerBindingMaxAttempts);
    }
}

bool ReconcileInitialOwnerBinding(const wchar_t* trigger) {
    auto& state = g_app.reconcile.initial_owner_binding;
    HWND overlay = g_app.window.overlay_hwnd;
    HWND target = state.target;
    HWND effective_owner = overlay && IsWindow(overlay) ? GetWindow(overlay, GW_OWNER) : nullptr;
    const bool overlay_valid = overlay && IsWindow(overlay);
    const bool target_valid =
        target &&
        target == CommittedTaskbarWindow() &&
        IsWindow(target) &&
        IsTaskbarWindowClass(target) &&
        WindowProcessId(target) == CommittedTaskbarProcessId();
    const std::uint64_t now_ms = GetTickCount64();
    const InitialOwnerBindingAction action = DecideInitialOwnerBindingAction(
        state.pending,
        overlay_valid,
        target_valid,
        effective_owner == target,
        state.attempts,
        kInitialOwnerBindingMaxAttempts,
        now_ms,
        state.next_retry_ms);

    if (action == InitialOwnerBindingAction::None ||
        action == InitialOwnerBindingAction::Wait) {
        return false;
    }
    if (action == InitialOwnerBindingAction::Complete) {
        LogInfo(
            L"event=overlay_owner_binding result=observed_bound trigger=%ls hwnd=%p target=%p attempts=%u generation=%llu",
            trigger,
            overlay,
            target,
            state.attempts,
            static_cast<unsigned long long>(g_app.diagnostics.overlay_generation));
        ResetInitialOwnerBindingState();
        return false;
    }
    if (action == InitialOwnerBindingAction::Exhausted) {
        state.pending = false;
        state.exhausted = true;
        LogWarning(
            L"event=overlay_owner_binding result=exhausted trigger=%ls hwnd=%p target=%p effective_owner=%p "
            L"attempts=%u generation=%llu",
            trigger,
            overlay,
            target,
            effective_owner,
            state.attempts,
            static_cast<unsigned long long>(g_app.diagnostics.overlay_generation));
        return false;
    }

    ++state.attempts;
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous_owner_value = SetWindowLongPtrW(
        overlay,
        GWLP_HWNDPARENT,
        reinterpret_cast<LONG_PTR>(target));
    const DWORD set_error =
        previous_owner_value == 0 ? GetLastError() : ERROR_SUCCESS;
    SetLastError(ERROR_SUCCESS);
    const BOOL positioned = SetWindowPos(
        overlay,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    const DWORD position_error = positioned ? ERROR_SUCCESS : GetLastError();
    effective_owner = GetWindow(overlay, GW_OWNER);
    state.next_retry_ms = now_ms + kInitialOwnerBindingRetryIntervalMs;
    if (effective_owner != target) {
        LogWarning(
            L"event=overlay_owner_binding result=retry_failed trigger=%ls hwnd=%p target=%p effective_owner=%p "
            L"attempt=%u max_attempts=%u set_error=%lu topmost_error=%lu generation=%llu",
            trigger,
            overlay,
            target,
            effective_owner,
            state.attempts,
            kInitialOwnerBindingMaxAttempts,
            set_error,
            position_error,
            static_cast<unsigned long long>(g_app.diagnostics.overlay_generation));
        return false;
    }
    LogInfo(
        L"event=overlay_owner_binding result=ok trigger=%ls hwnd=%p target=%p effective_owner=%p "
        L"attempt=%u set_error=%lu topmost_error=%lu generation=%llu",
        trigger,
        overlay,
        target,
        effective_owner,
        state.attempts,
        set_error,
        position_error,
        static_cast<unsigned long long>(g_app.diagnostics.overlay_generation));
    ResetInitialOwnerBindingState();
    return true;
}

void ResetOverlayPresentDiagnostics() {
    g_app.diagnostics.last_present_attempt_tick = 0;
    g_app.diagnostics.last_present_success_tick = 0;
    g_app.diagnostics.overlay_created_tick = 0;
}

bool DestroyOverlayWindow(const wchar_t* reason) {
    HWND overlay = g_app.window.overlay_hwnd;
    if (!overlay || !IsWindow(overlay)) {
        g_app.window.overlay_hwnd = nullptr;
        g_app.placement.taskbar_owner = nullptr;
        ResetInitialOwnerBindingState();
        ResetOverlayPresentDiagnostics();
        return true;
    }

    LogInfo(L"event=overlay_destroy_requested reason=%ls hwnd=%p", reason, overlay);
    g_app.window.overlay_destroy_expected = true;
    const BOOL destroyed = DestroyWindow(overlay);
    const DWORD destroy_error = destroyed ? ERROR_SUCCESS : GetLastError();
    g_app.window.overlay_destroy_expected = false;
    if (!destroyed && IsWindow(overlay)) {
        LogError(L"event=overlay_destroy_failed error=%lu", destroy_error);
        return false;
    }
    g_app.window.overlay_hwnd = nullptr;
    g_app.placement.taskbar_owner = nullptr;
    ResetInitialOwnerBindingState();
    ResetOverlayPresentDiagnostics();
    return true;
}

bool EnsureOverlayWindow(DWORD* error_out) {
    if (error_out) {
        *error_out = ERROR_SUCCESS;
    }
    HWND taskbar = CommittedTaskbarWindow();
    const bool taskbar_valid =
        taskbar &&
        IsWindow(taskbar) &&
        IsTaskbarWindowClass(taskbar) &&
        WindowProcessId(taskbar) == CommittedTaskbarProcessId();
    HWND overlay = g_app.window.overlay_hwnd;
    if (taskbar_valid &&
        overlay &&
        IsWindow(overlay) &&
        g_app.placement.taskbar_owner == taskbar) {
        return true;
    }

    if (overlay && IsWindow(overlay)) {
        if (error_out) {
            *error_out = ERROR_INVALID_STATE;
        }
        LogErrorRateLimited(
            L"overlay.owner_generation_mismatch",
            kFailureLogIntervalMs,
            L"event=overlay_create_blocked reason=owner_generation_mismatch hwnd=%p cached_owner=%p committed_owner=%p",
            overlay,
            g_app.placement.taskbar_owner,
            taskbar);
        return false;
    }
    g_app.window.overlay_hwnd = nullptr;
    g_app.placement.taskbar_owner = nullptr;
    ResetInitialOwnerBindingState();
    ResetOverlayPresentDiagnostics();

    if (!taskbar_valid) {
        if (error_out) {
            *error_out = ERROR_NOT_FOUND;
        }
        LogWarningRateLimited(
            L"overlay.taskbar_missing",
            kFailureLogIntervalMs,
            L"event=overlay_unavailable reason=taskbar_missing");
        return false;
    }
    LogFailureRecovered(
        L"overlay.taskbar_missing",
        L"event=component_recovered component=overlay reason=taskbar_available");

    const DWORD ex_style =
        WS_EX_TOOLWINDOW |
        WS_EX_TOPMOST |
        WS_EX_LAYERED |
        WS_EX_NOACTIVATE;
    overlay = CreateWindowExW(
        ex_style,
        kOverlayWindowClass,
        L"Simple Monitor",
        WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        430,
        44,
        taskbar,
        nullptr,
        g_app.window.instance,
        nullptr);
    if (!overlay) {
        const DWORD create_error = GetLastError();
        if (error_out) {
            *error_out = create_error;
        }
        LogErrorRateLimited(
            L"overlay.create_window",
            kFailureLogIntervalMs,
            L"event=overlay_create_failed error=%lu",
            create_error);
        return false;
    }
    LogFailureRecovered(
        L"overlay.create_window",
        L"event=component_recovered component=overlay stage=create_window");

    g_app.window.overlay_hwnd = overlay;
    ResetOverlayPresentDiagnostics();
    ++g_app.diagnostics.overlay_generation;
    const DWORD created_tick = GetTickCount();
    g_app.diagnostics.overlay_created_tick = created_tick == 0 ? 1 : created_tick;
    g_app.window.dpi = WindowDpi(overlay);
    g_app.placement.taskbar_owner = taskbar;
    UpdateLayeredStyle(overlay);
    const BOOL disable_transitions = TRUE;
    const HRESULT transition_hresult = DwmSetWindowAttribute(
        overlay,
        DWMWA_TRANSITIONS_FORCEDISABLED,
        &disable_transitions,
        sizeof(disable_transitions));
    if (SUCCEEDED(transition_hresult)) {
        LogInfo(
            L"event=overlay_dwm_attribute result=ok attribute=transitions_forced_disabled generation=%llu",
            static_cast<unsigned long long>(g_app.diagnostics.overlay_generation));
    } else {
        LogWarningRateLimited(
            L"overlay.dwm_disable_transitions",
            kFailureLogIntervalMs,
            L"event=overlay_dwm_attribute result=failed attribute=transitions_forced_disabled hresult=0x%08lx",
            static_cast<unsigned long>(transition_hresult));
    }
    const HWND effective_owner = GetWindow(overlay, GW_OWNER);
    LogInfo(
        L"event=overlay_created hwnd=%p requested_owner=%p effective_owner=%p owner_bound=%d generation=%llu",
        overlay,
        taskbar,
        effective_owner,
        effective_owner == taskbar ? 1 : 0,
        static_cast<unsigned long long>(g_app.diagnostics.overlay_generation));
    ArmInitialOwnerBindingVerification(
        overlay,
        taskbar,
        L"overlay_create",
        kInitialOwnerBindingRetryIntervalMs);
    return true;
}

simple_monitor::overlay_policy::TaskbarIdentityResult UpdateTaskbarIdentity(
    const wchar_t* trigger) {
    const TaskbarIdentity observed = CurrentTaskbarIdentity();
    const TaskbarIdentityState previous_state = g_app.placement.taskbar_identity;
    auto result = ReduceTaskbarIdentity(
        previous_state,
        observed,
        GetTickCount64(),
        kTaskbarIdentitySettleMs);
    g_app.placement.taskbar_identity = result.state;

    const bool candidate_started =
        result.state.candidate_active &&
        (!previous_state.candidate_active || previous_state.candidate != result.state.candidate);
    if (candidate_started) {
        LogInfo(
            L"event=taskbar_identity_candidate trigger=%ls old_hwnd=%p new_hwnd=%p old_pid=%lu new_pid=%lu",
            trigger,
            TaskbarIdentityWindow(previous_state.committed),
            TaskbarIdentityWindow(result.state.candidate),
            static_cast<DWORD>(previous_state.committed.process_id),
            static_cast<DWORD>(result.state.candidate.process_id));
    }

    if (result.committed_changed) {
        LogInfo(
            L"event=taskbar_identity_committed trigger=%ls old_hwnd=%p new_hwnd=%p old_pid=%lu new_pid=%lu dwell_ms=%llu",
            trigger,
            TaskbarIdentityWindow(result.previous_committed),
            TaskbarIdentityWindow(result.state.committed),
            static_cast<DWORD>(result.previous_committed.process_id),
            static_cast<DWORD>(result.state.committed.process_id),
            static_cast<unsigned long long>(result.candidate_dwell_ms));
    }
    return result;
}

void RepairOverlayInvariant(const wchar_t* trigger, const OverlayStateSnapshot& before) {
    if (before.issues == OverlayInvariantNone) {
        LogFailureRecovered(
            L"overlay.invariant",
            L"event=overlay_invariant_recovered trigger=%ls",
            trigger);
        if (g_app.diagnostics.last_present_success_tick != 0) {
            LogFailureRecovered(
                L"overlay.repair",
                L"event=component_recovered component=overlay_repair trigger=%ls",
                trigger);
        }
        return;
    }

    const DWORD now = GetTickCount();
    const bool refresh_only = before.issues == OverlayInvariantPresentStale;
    if (!refresh_only && ShouldLogOverlayInvariantDetail(now)) {
        LogOverlayInvariantFailure(trigger, before);
    }

    constexpr unsigned kActionableIssues =
        OverlayInvariantMissing |
        OverlayInvariantHidden |
        OverlayInvariantNotTopmost |
        OverlayInvariantMissingLayeredStyle |
        OverlayInvariantInvalidRect |
        OverlayInvariantPresentStale;
    if ((before.issues & kActionableIssues) == 0) {
        // Cloaking is controlled by DWM and can be transient. Keep recording it,
        // but do not churn the HWND while the shell owns that state.
        return;
    }

    if (g_app.diagnostics.last_overlay_repair_tick != 0 &&
        !TickPassed(
            now,
            g_app.diagnostics.last_overlay_repair_tick + kOverlayRepairIntervalMs)) {
        // Detection stays frequent, while repair work is bounded. The normal
        // refresh/metrics path continues after this function returns.
        return;
    }
    g_app.diagnostics.last_overlay_repair_tick = now == 0 ? 1 : now;
    if (refresh_only) {
        LogInfo(
            L"event=overlay_refresh_requested trigger=%ls reason=present_stale last_present_age_ms=%lld",
            trigger,
            TickAgeMs(now, g_app.diagnostics.last_present_success_tick));
    }
    const std::uint64_t present_sequence_before_repair =
        g_app.diagnostics.present_success_sequence;

    constexpr unsigned kRepairEnsure = 1U << 0;
    constexpr unsigned kRepairStyle = 1U << 1;
    constexpr unsigned kRepairPosition = 1U << 2;
    constexpr unsigned kRepairPresent = 1U << 3;
    constexpr unsigned kRepairCommitVisible = 1U << 4;
    const auto plan = ComputeOverlayRepairIntent({
        before.valid,
        before.visible,
        (before.ex_style & WS_EX_TOPMOST) != 0,
        (before.ex_style & WS_EX_LAYERED) != 0,
        before.valid && (before.issues & OverlayInvariantInvalidRect) == 0,
        (before.issues & OverlayInvariantPresentStale) != 0,
    });

    unsigned actions = 0;
    DWORD repair_error = ERROR_SUCCESS;
    HWND old_overlay = before.hwnd;
    bool created = false;

    if (plan.ensure_exists) {
        actions |= kRepairEnsure;
        const std::uint64_t generation_before_ensure =
            g_app.diagnostics.overlay_generation;
        DWORD ensure_error = ERROR_SUCCESS;
        if (!EnsureOverlayWindow(&ensure_error)) {
            repair_error = ensure_error;
        }
        created =
            g_app.diagnostics.overlay_generation != generation_before_ensure;
    }

    HWND overlay = g_app.window.overlay_hwnd;
    if (overlay && IsWindow(overlay)) {
        const bool needs_visibility_commit =
            created || IsWindowVisible(overlay) == FALSE;
        if (needs_visibility_commit) {
            actions |=
                kRepairStyle |
                kRepairPosition |
                kRepairPresent |
                kRepairCommitVisible;
            if (!PrepareOverlayForShow(overlay, trigger, false)) {
                repair_error = ERROR_GEN_FAILURE;
            }
        } else {
            bool can_present = true;
            if (plan.apply_style) {
                actions |= kRepairStyle;
                if (!UpdateLayeredStyle(overlay)) {
                    repair_error = ERROR_GEN_FAILURE;
                    can_present = false;
                }
            }

            bool geometry_changed = false;
            if (plan.reposition) {
                actions |= kRepairPosition;
                geometry_changed = RepositionWindow();
            }

            if (can_present && (plan.present || geometry_changed)) {
                actions |= kRepairPresent;
                RenderOverlay(overlay);
            }
        }
    }

    if (actions == 0) {
        return;
    }

    OverlayStateSnapshot after = CaptureOverlayState(true);
    const bool presentation_required = (actions & kRepairPresent) != 0;
    const bool presented_after_repair =
        !presentation_required ||
        g_app.diagnostics.present_success_sequence > present_sequence_before_repair;
    if (!presented_after_repair) {
        after.issues |= OverlayInvariantPresentStale;
    }
    const bool repaired =
        presented_after_repair &&
        (after.issues & kActionableIssues) == OverlayInvariantNone;
    if (repaired) {
        if (refresh_only) {
            ++g_app.diagnostics.overlay_refreshes;
            LogFailureRecovered(
                L"overlay.refresh",
                L"event=component_recovered component=overlay_refresh trigger=%ls",
                trigger);
            LogInfo(
                L"event=overlay_refresh trigger=%ls result=ok actions=0x%02x presented=%d visibility_commit=%d duration_ms=%lu",
                trigger,
                actions,
                presented_after_repair ? 1 : 0,
                (actions & kRepairCommitVisible) != 0 ? 1 : 0,
                GetTickCount() - now);
            return;
        }

        ++g_app.diagnostics.overlay_repairs;
        if (after.issues == OverlayInvariantNone) {
            LogFailureRecovered(
                L"overlay.invariant",
                L"event=overlay_invariant_recovered trigger=%ls",
                trigger);
        }
        LogFailureRecovered(
            L"overlay.repair",
            L"event=component_recovered component=overlay_repair trigger=%ls",
            trigger);
        LogInfo(
            L"event=overlay_repair trigger=%ls result=ok issues=0x%02x remaining=0x%02x actions=0x%02x "
            L"presented=%d visibility_commit=%d old_hwnd=%p new_hwnd=%p duration_ms=%lu",
            trigger,
            before.issues,
            after.issues,
            actions,
            presented_after_repair ? 1 : 0,
            (actions & kRepairCommitVisible) != 0 ? 1 : 0,
            old_overlay,
            after.hwnd,
            GetTickCount() - now);
    } else {
        if (refresh_only) {
            ++g_app.diagnostics.overlay_refresh_failures;
            LogErrorRateLimited(
                L"overlay.refresh",
                kFailureLogIntervalMs,
                L"event=overlay_refresh trigger=%ls result=failed actions=0x%02x presented=%d visibility_commit=%d error=%lu",
                trigger,
                actions,
                presented_after_repair ? 1 : 0,
                (actions & kRepairCommitVisible) != 0 ? 1 : 0,
                repair_error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : repair_error);
            return;
        }

        ++g_app.diagnostics.overlay_repair_failures;
        const DWORD reported_error =
            repair_error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : repair_error;
        LogErrorRateLimited(
            L"overlay.repair",
            kFailureLogIntervalMs,
            L"event=overlay_repair trigger=%ls result=failed issues=0x%02x remaining=0x%02x "
            L"actions=0x%02x presented=%d visibility_commit=%d old_hwnd=%p new_hwnd=%p error=%lu",
            trigger,
            before.issues,
            after.issues,
            actions,
            presented_after_repair ? 1 : 0,
            (actions & kRepairCommitVisible) != 0 ? 1 : 0,
            old_overlay,
            after.hwnd,
            reported_error);
    }
}

void MaybeLogHealth(const wchar_t* trigger, bool force = false) {
    if (!simple_monitor::IsLogEnabled(LogLevel::Info)) {
        return;
    }

    const DWORD now = GetTickCount();
    if (!force &&
        g_app.diagnostics.last_health_log_tick != 0 &&
        !TickPassed(now, g_app.diagnostics.last_health_log_tick + kHealthLogIntervalMs)) {
        return;
    }
    g_app.diagnostics.last_health_log_tick = now;

    HWND committed_taskbar = CommittedTaskbarWindow();
    const bool taskbar_ready =
        committed_taskbar &&
        IsWindow(committed_taskbar) &&
        WindowProcessId(committed_taskbar) == CommittedTaskbarProcessId();
    const OverlayIntent intent = ComputeOverlayIntent(
        taskbar_ready,
        g_app.suppression.policy.committed,
        g_app.suppression.overlay_update_frozen);
    const bool expected_visible =
        g_app.window.monitor_initialized && intent.should_be_visible;
    const OverlayStateSnapshot state = CaptureOverlayState(expected_visible);
    const std::wstring above_exe = WindowProcessBasename(state.above);
    const std::wstring above_class = WindowClassName(state.above);

    LogInfo(
        L"event=health trigger=%ls controller=%p monitor_initialized=%d taskbar=%p taskbar_pid=%lu "
        L"overlay=%p generation=%llu valid=%d visible=%d cloak_known=%d cloak_hresult=0x%08lx "
        L"cloak_flags=0x%08lx topmost=%d layered=%d owner=%p expected_owner=%p "
        L"owner_bind_pending=%d owner_bind_exhausted=%d owner_bind_attempts=%u owner_bind_target=%p "
        L"rect=(%ld,%ld,%ld,%ld) above=%p above_exe=%ls above_class=%ls issues=0x%02x "
        L"suppression=%ls candidate=%ls candidate_active=%d candidate_age_ms=%lld "
        L"desired_visible=%d visibility_matches=%d decision=%llu frozen=%d "
        L"frame_size=(%d,%d) frame_visible=%d "
        L"last_present_attempt_age_ms=%lld last_present_age_ms=%lld "
        L"last_render_attempt_age_ms=%lld last_render_age_ms=%lld last_reposition_age_ms=%lld "
        L"last_metrics_age_ms=%lld state_gap_ms=%lu present_failures=%u render_failures=%u "
        L"repairs=%u repair_failures=%u refreshes=%u refresh_failures=%u",
        trigger,
        g_app.window.controller_hwnd,
        g_app.window.monitor_initialized ? 1 : 0,
        state.expected_owner,
        WindowProcessId(state.expected_owner),
        state.hwnd,
        static_cast<unsigned long long>(g_app.diagnostics.overlay_generation),
        state.valid ? 1 : 0,
        state.visible ? 1 : 0,
        state.cloak_known ? 1 : 0,
        static_cast<unsigned long>(state.cloak_hresult),
        static_cast<unsigned long>(state.cloak_flags),
        (state.ex_style & WS_EX_TOPMOST) != 0 ? 1 : 0,
        (state.ex_style & WS_EX_LAYERED) != 0 ? 1 : 0,
        state.owner,
        state.expected_owner,
        g_app.reconcile.initial_owner_binding.pending ? 1 : 0,
        g_app.reconcile.initial_owner_binding.exhausted ? 1 : 0,
        g_app.reconcile.initial_owner_binding.attempts,
        g_app.reconcile.initial_owner_binding.target,
        state.rect.left,
        state.rect.top,
        state.rect.right,
        state.rect.bottom,
        state.above,
        above_exe.c_str(),
        above_class.c_str(),
        state.issues,
        SuppressionReasonName(g_app.suppression.policy.committed),
        SuppressionReasonName(g_app.suppression.policy.candidate),
        g_app.suppression.policy.candidate_active ? 1 : 0,
        g_app.suppression.policy.candidate_active
            ? static_cast<long long>(
                  GetTickCount64() - g_app.suppression.policy.candidate_since_ms)
            : -1LL,
        expected_visible ? 1 : 0,
        state.valid && state.visible == expected_visible ? 1 : 0,
        static_cast<unsigned long long>(g_app.suppression.decision_sequence),
        g_app.suppression.overlay_update_frozen ? 1 : 0,
        g_app.render.last_frame_width,
        g_app.render.last_frame_height,
        g_app.diagnostics.last_frame_has_visible_pixels ? 1 : 0,
        TickAgeMs(now, g_app.diagnostics.last_present_attempt_tick),
        TickAgeMs(now, g_app.diagnostics.last_present_success_tick),
        TickAgeMs(now, g_app.diagnostics.last_render_attempt_tick),
        TickAgeMs(now, g_app.diagnostics.last_render_success_tick),
        TickAgeMs(now, g_app.diagnostics.last_reposition_success_tick),
        TickAgeMs(now, g_app.diagnostics.last_metrics_sample_tick),
        g_app.diagnostics.last_state_timer_gap_ms,
        g_app.diagnostics.present_failures,
        g_app.diagnostics.render_failures,
        g_app.diagnostics.overlay_repairs,
        g_app.diagnostics.overlay_repair_failures,
        g_app.diagnostics.overlay_refreshes,
        g_app.diagnostics.overlay_refresh_failures);
}

void ReloadConfigAndRefresh() {
    LogInfo(L"event=config_reload_requested");
    const simple_monitor::Config previous_config = g_app.config;
    const simple_monitor::Config next_config = simple_monitor::LoadConfig();
    LogLoggingConfigChange(previous_config, next_config);
    if (previous_config.debug_log && !next_config.debug_log) {
        simple_monitor::FlushLogSummaries();
    }
    ApplyAppConfig(next_config);
    if (!previous_config.debug_log && next_config.debug_log) {
        simple_monitor::ResetLog();
    }
    LogConfigSnapshot(L"config_reloaded");
    ReconcileOverlayState(
        L"config_reload",
        OverlayReconcileReposition | OverlayReconcileRender);
}

void RecoverTaskbar() {
    LogInfo(L"event=manual_taskbar_recovery");
    ArmInitialOwnerBindingVerification(
        g_app.window.overlay_hwnd,
        CommittedTaskbarWindow(),
        L"manual_recovery",
        0);
    ReconcileOverlayState(
        L"manual_recovery",
        OverlayReconcileReposition |
            OverlayReconcileRender |
            OverlayReconcileResetSurface |
            OverlayReconcileRefreshTrayIcon);
    LogOverlayWindowState(L"manual_recovery_complete", g_app.window.overlay_hwnd);
    MaybeLogHealth(L"manual_recovery", true);
}

bool HandleMenuCommand(HWND hwnd, UINT command) {
    switch (command) {
    case ID_OPEN_CONFIG: {
        const HINSTANCE result = ShellExecuteW(
            hwnd,
            L"open",
            ConfigPath().c_str(),
            nullptr,
            ModuleDir().c_str(),
            SW_SHOWNORMAL);
        const INT_PTR code = reinterpret_cast<INT_PTR>(result);
        if (code <= 32) {
            LogWarning(L"event=user_action action=open_config result=failed code=%lld", static_cast<long long>(code));
        } else {
            LogInfo(L"event=user_action action=open_config result=ok");
        }
        return true;
    }
    case ID_RELOAD_CONFIG:
        ReloadConfigAndRefresh();
        return true;
    case ID_RECOVER_TASKBAR:
        RecoverTaskbar();
        return true;
    case ID_CLICK_THROUGH:
        g_app.tray.click_through = !g_app.tray.click_through;
        ReconcileOverlayState(L"click_through", OverlayReconcileApplyStyle);
        if (g_app.window.overlay_hwnd &&
            (((GetWindowLongPtrW(g_app.window.overlay_hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0) ==
             g_app.tray.click_through)) {
            LogInfo(
                L"event=user_action action=click_through result=ok enabled=%d",
                g_app.tray.click_through ? 1 : 0);
        } else if (((g_app.reconcile.deferred_flags | g_app.reconcile.pending_flags) &
                    OverlayReconcileApplyStyle) != 0 ||
                   g_app.reconcile.active) {
            LogInfo(
                L"event=user_action action=click_through result=pending enabled=%d",
                g_app.tray.click_through ? 1 : 0);
        } else {
            LogError(
                L"event=user_action action=click_through result=failed requested_enabled=%d",
                g_app.tray.click_through ? 1 : 0);
        }
        return true;
    case ID_STARTUP: {
        const bool requested = !IsStartupEnabled();
        const bool succeeded = SetStartupEnabled(requested);
        LogInfo(
            L"event=user_action action=startup requested=%d result=%ls effective=%d",
            requested ? 1 : 0,
            succeeded ? L"ok" : L"failed",
            IsStartupEnabled() ? 1 : 0);
        return true;
    }
    case ID_EXIT:
        g_app.window.shutdown_trigger = L"user_menu";
        LogInfo(L"event=user_action action=exit result=requested");
        if (!DestroyWindow(g_app.window.controller_hwnd)) {
            g_app.window.shutdown_trigger = L"window_destroy";
            LogError(L"event=user_action action=exit result=failed error=%lu", GetLastError());
        }
        return true;
    }
    return false;
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);

    bool keep_open = true;
    while (keep_open) {
        HMENU menu = CreatePopupMenu();
        if (!menu) {
            return;
        }

        AppendMenuW(menu, MF_STRING, ID_OPEN_CONFIG, L"Open config");
        AppendMenuW(menu, MF_STRING, ID_RELOAD_CONFIG, L"Reload config");
        AppendMenuW(menu, MF_STRING, ID_RECOVER_TASKBAR, L"Recover taskbar");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (g_app.tray.click_through ? MF_CHECKED : MF_UNCHECKED), ID_CLICK_THROUGH, L"Click-through");
        AppendMenuW(menu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : MF_UNCHECKED), ID_STARTUP, L"Start with Windows");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

        const UINT command = TrackPopupMenu(
            menu,
            TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN | TPM_RETURNCMD | TPM_NOANIMATION,
            pt.x,
            pt.y,
            0,
            hwnd,
            nullptr);
        DestroyMenu(menu);

        if (command == 0 || !HandleMenuCommand(hwnd, command)) {
            break;
        }
        keep_open = command == ID_CLICK_THROUGH || command == ID_STARTUP;
    }
}

// DirectWrite/Direct2D text layout and rendering helpers.
void ReportRenderFailure(
    const wchar_t* key,
    const wchar_t* stage,
    HRESULT hr,
    int width = 0,
    int height = 0) {
    ++g_app.diagnostics.render_failures;
    LogErrorRateLimited(
        key,
        kFailureLogIntervalMs,
        L"event=render_failed stage=%ls hresult=0x%08lx width=%d height=%d dpi=%u failures=%u",
        stage,
        static_cast<unsigned long>(hr),
        width,
        height,
        g_app.window.dpi,
        g_app.diagnostics.render_failures);
}

void ReportRenderRecovered(const wchar_t* key, const wchar_t* stage) {
    LogFailureRecovered(
        key,
        L"event=component_recovered component=render stage=%ls",
        stage);
}

HRESULT EnsureRenderResources() {
    RenderResources& render = g_app.render.resources;
    HRESULT hr = S_OK;
    const bool dpi_changed = render.text_format_dpi != g_app.window.dpi;

    if (!render.d2d_factory) {
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &render.d2d_factory);
        if (FAILED(hr)) {
            ReportRenderFailure(L"render.d2d_factory", L"d2d_factory", hr);
            return hr;
        }
        ReportRenderRecovered(L"render.d2d_factory", L"d2d_factory");
    }

    if (!render.dwrite_factory) {
        IUnknown* factory = nullptr;
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &factory);
        if (FAILED(hr)) {
            ReportRenderFailure(L"render.dwrite_factory", L"dwrite_factory", hr);
            return hr;
        }
        render.dwrite_factory = static_cast<IDWriteFactory*>(factory);
        ReportRenderRecovered(L"render.dwrite_factory", L"dwrite_factory");
    }

    if (!render.wic_factory) {
        hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            __uuidof(IWICImagingFactory),
            reinterpret_cast<void**>(&render.wic_factory));
        if (FAILED(hr)) {
            ReportRenderFailure(L"render.wic_factory", L"wic_factory", hr);
            return hr;
        }
        ReportRenderRecovered(L"render.wic_factory", L"wic_factory");
    }

    if (!render.text_format ||
        dpi_changed ||
        render.text_format_font_size_dip != g_app.config.font_size_dip) {
        SafeRelease(render.text_format);
        hr = render.dwrite_factory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            static_cast<FLOAT>(Scale(g_app.config.font_size_dip, g_app.window.dpi)),
            L"",
            &render.text_format);
        if (FAILED(hr)) {
            ReportRenderFailure(L"render.text_format_main", L"text_format_main", hr);
            return hr;
        }
        ReportRenderRecovered(L"render.text_format_main", L"text_format_main");

        render.text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        render.text_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        render.text_format_font_size_dip = g_app.config.font_size_dip;
    }

    if (!render.arrow_text_format ||
        dpi_changed ||
        render.arrow_text_format_font_size_dip != g_app.config.network_arrow_font_size_dip) {
        SafeRelease(render.arrow_text_format);
        hr = render.dwrite_factory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            static_cast<FLOAT>(Scale(g_app.config.network_arrow_font_size_dip, g_app.window.dpi)),
            L"",
            &render.arrow_text_format);
        if (FAILED(hr)) {
            ReportRenderFailure(L"render.text_format_arrow", L"text_format_arrow", hr);
            return hr;
        }
        ReportRenderRecovered(L"render.text_format_arrow", L"text_format_arrow");

        render.arrow_text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        render.arrow_text_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        render.arrow_text_format_font_size_dip = g_app.config.network_arrow_font_size_dip;
    }

    if (!render.key_text_format ||
        dpi_changed ||
        render.key_text_format_font_size_dip != g_app.config.key_font_size_dip) {
        SafeRelease(render.key_text_format);
        hr = render.dwrite_factory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            static_cast<FLOAT>(Scale(g_app.config.key_font_size_dip, g_app.window.dpi)),
            L"",
            &render.key_text_format);
        if (FAILED(hr)) {
            ReportRenderFailure(L"render.text_format_key", L"text_format_key", hr);
            return hr;
        }
        ReportRenderRecovered(L"render.text_format_key", L"text_format_key");

        render.key_text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        render.key_text_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        render.key_text_format_font_size_dip = g_app.config.key_font_size_dip;
    }

    render.text_format_dpi = g_app.window.dpi;

    return S_OK;
}

void ReleaseRenderResources() {
    SafeRelease(g_app.render.resources.key_text_format);
    SafeRelease(g_app.render.resources.arrow_text_format);
    SafeRelease(g_app.render.resources.text_format);
    SafeRelease(g_app.render.resources.wic_factory);
    SafeRelease(g_app.render.resources.dwrite_factory);
    SafeRelease(g_app.render.resources.d2d_factory);
}

void DrawTextCellWithFormat(
    ID2D1RenderTarget* target,
    ID2D1SolidColorBrush* brush,
    IDWriteTextFormat* format,
    const D2D1_RECT_F& rect,
    const std::wstring& text,
    DWRITE_TEXT_ALIGNMENT alignment) {
    format->SetTextAlignment(alignment);
    target->DrawTextW(
        text.c_str(),
        static_cast<UINT32>(text.size()),
        format,
        rect,
        brush,
        D2D1_DRAW_TEXT_OPTIONS_CLIP,
        DWRITE_MEASURING_MODE_NATURAL);
}

void DrawTextCell(
    ID2D1RenderTarget* target,
    ID2D1SolidColorBrush* brush,
    const D2D1_RECT_F& rect,
    const std::wstring& text,
    DWRITE_TEXT_ALIGNMENT alignment) {
    DrawTextCellWithFormat(target, brush, g_app.render.resources.text_format, rect, text, alignment);
}

FLOAT MeasureTextWidthWithFormat(IDWriteTextFormat* format, const std::wstring& text) {
    IDWriteTextLayout* layout = nullptr;
    HRESULT hr = g_app.render.resources.dwrite_factory->CreateTextLayout(
        text.c_str(),
        static_cast<UINT32>(text.size()),
        format,
        10000.0f,
        1000.0f,
        &layout);
    if (FAILED(hr) || !layout) {
        return 1.0f;
    }

    DWRITE_TEXT_METRICS metrics{};
    hr = layout->GetMetrics(&metrics);
    SafeRelease(layout);
    if (FAILED(hr)) {
        return 1.0f;
    }

    return std::max(1.0f, metrics.widthIncludingTrailingWhitespace + 1.0f);
}

FLOAT MeasureTextWidth(const std::wstring& text) {
    return MeasureTextWidthWithFormat(g_app.render.resources.text_format, text);
}

struct TextColumn {
    std::wstring top;
    std::wstring bottom;
    std::wstring width_sample;
    FLOAT gap_after = -1.0f;
    FLOAT width = 1.0f;
    bool split_prefix = false;
    bool key_widget = false;
    std::wstring top_prefix;
    std::wstring top_value;
    std::wstring bottom_prefix;
    std::wstring bottom_value;
};

std::wstring NetworkArrow(bool upload) {
    const std::wstring& style = g_app.config.network_arrow_style;
    if (style == L"triangle") {
        return upload ? L"▲" : L"▼";
    }
    if (style == L"heavy") {
        return upload ? L"⬆" : L"⬇";
    }
    if (style == L"chevron") {
        return upload ? L"▴" : L"▾";
    }
    return upload ? L"↑" : L"↓";
}

TextColumn MakeTextColumn(
    const std::wstring& top,
    const std::wstring& bottom,
    const std::wstring& width_sample,
    FLOAT gap_after = -1.0f) {
    TextColumn column{};
    column.top = top;
    column.bottom = bottom;
    column.width_sample = width_sample;
    column.gap_after = gap_after;
    return column;
}

TextColumn MakeKeyWidgetColumn(FLOAT gap_after = -1.0f) {
    TextColumn column{};
    column.key_widget = true;
    column.top = L"CAP INS NUM";
    column.bottom = L"";
    column.width_sample = L"CAP INS NUM";
    column.gap_after = gap_after;
    return column;
}

TextColumn MakeNetworkColumn(const std::wstring& up_value, const std::wstring& down_value, FLOAT gap_after) {
    TextColumn column{};
    column.split_prefix = true;
    column.top_prefix = NetworkArrow(true);
    column.top_value = up_value;
    column.bottom_prefix = NetworkArrow(false);
    column.bottom_value = down_value;
    column.top = column.top_prefix + L": " + column.top_value;
    column.bottom = column.bottom_prefix + L": " + column.bottom_value;
    column.width_sample = NetworkArrow(false) + L": 99.9MB/s";
    column.gap_after = gap_after;
    return column;
}

FLOAT MeasureSplitLineWidth(const TextColumn& column, bool top) {
    const std::wstring& prefix = top ? column.top_prefix : column.bottom_prefix;
    const std::wstring& value = top ? column.top_value : column.bottom_value;
    return MeasureTextWidthWithFormat(g_app.render.resources.arrow_text_format, prefix) +
           MeasureTextWidth(L":") +
           static_cast<FLOAT>(Scale(g_app.config.network_arrow_gap_dip, g_app.window.dpi)) +
           MeasureTextWidth(value);
}

void DrawSplitLine(
    ID2D1RenderTarget* target,
    ID2D1SolidColorBrush* brush,
    const D2D1_RECT_F& rect,
    const std::wstring& prefix,
    const std::wstring& value) {
    const FLOAT prefix_width = MeasureTextWidthWithFormat(g_app.render.resources.arrow_text_format, prefix);
    const FLOAT colon_width = MeasureTextWidth(L":");
    const FLOAT gap = static_cast<FLOAT>(Scale(g_app.config.network_arrow_gap_dip, g_app.window.dpi));
    D2D1_RECT_F prefix_rect{rect.left, rect.top, rect.left + prefix_width, rect.bottom};
    D2D1_RECT_F colon_rect{prefix_rect.right, rect.top, prefix_rect.right + colon_width, rect.bottom};
    D2D1_RECT_F value_rect{colon_rect.right + gap, rect.top, rect.right, rect.bottom};
    DrawTextCellWithFormat(target, brush, g_app.render.resources.arrow_text_format, prefix_rect, prefix, DWRITE_TEXT_ALIGNMENT_LEADING);
    DrawTextCell(target, brush, colon_rect, L":", DWRITE_TEXT_ALIGNMENT_LEADING);
    DrawTextCell(target, brush, value_rect, value, DWRITE_TEXT_ALIGNMENT_LEADING);
}

void DrawKeyToken(
    ID2D1RenderTarget* target,
    ID2D1SolidColorBrush* brush,
    const D2D1_RECT_F& rect,
    const std::wstring& text,
    bool active) {
    brush->SetColor(active ? D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 1.0f} : D2D1_COLOR_F{0.45f, 0.45f, 0.45f, 1.0f});
    DrawTextCellWithFormat(target, brush, g_app.render.resources.key_text_format, rect, text, DWRITE_TEXT_ALIGNMENT_LEADING);
    brush->SetColor(D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 1.0f});
}

void DrawKeyWidget(
    ID2D1RenderTarget* target,
    ID2D1SolidColorBrush* brush,
    const D2D1_RECT_F& top_rect,
    const D2D1_RECT_F& bottom_rect) {
    const FLOAT cap_width = MeasureTextWidthWithFormat(g_app.render.resources.key_text_format, L"CAP");
    const FLOAT ins_width = MeasureTextWidthWithFormat(g_app.render.resources.key_text_format, L"INS");
    const FLOAT num_width = MeasureTextWidthWithFormat(g_app.render.resources.key_text_format, L"NUM");
    const FLOAT token_gap = static_cast<FLOAT>(Scale(5, g_app.window.dpi));
    const FLOAT required_width = cap_width + ins_width + num_width + token_gap * 2.0f;
    D2D1_RECT_F row_rect{top_rect.left, top_rect.top, top_rect.left + required_width, bottom_rect.bottom};
    D2D1_RECT_F cap_rect{row_rect.left, row_rect.top, row_rect.left + cap_width, row_rect.bottom};
    D2D1_RECT_F ins_rect{cap_rect.right + token_gap, row_rect.top, cap_rect.right + token_gap + ins_width, row_rect.bottom};
    D2D1_RECT_F num_rect{ins_rect.right + token_gap, row_rect.top, ins_rect.right + token_gap + num_width, row_rect.bottom};
    DrawKeyToken(target, brush, cap_rect, L"CAP", g_app.metrics.current.caps);
    DrawKeyToken(target, brush, ins_rect, L"INS", g_app.metrics.current.insert);
    DrawKeyToken(target, brush, num_rect, L"NUM", g_app.metrics.current.num);
}

void DrawAdaptiveColumnsDwrite(
    ID2D1RenderTarget* target,
    ID2D1SolidColorBrush* brush,
    const D2D1_RECT_F& rect,
    std::vector<TextColumn> columns) {
    if (columns.empty()) {
        return;
    }

    FLOAT total_text_width = 0.0f;
    for (TextColumn& column : columns) {
        if (column.key_widget) {
            column.width =
                MeasureTextWidthWithFormat(g_app.render.resources.key_text_format, L"CAP") +
                MeasureTextWidthWithFormat(g_app.render.resources.key_text_format, L"INS") +
                MeasureTextWidthWithFormat(g_app.render.resources.key_text_format, L"NUM") +
                static_cast<FLOAT>(Scale(10, g_app.window.dpi));
        } else if (column.split_prefix) {
            column.width = std::max(MeasureSplitLineWidth(column, true), MeasureSplitLineWidth(column, false));
        } else {
            column.width = std::max(MeasureTextWidth(column.top), MeasureTextWidth(column.bottom));
        }
        if (!column.width_sample.empty()) {
            if (column.split_prefix) {
                column.width = std::max(
                    column.width,
                    MeasureTextWidthWithFormat(g_app.render.resources.arrow_text_format, NetworkArrow(false)) +
                        MeasureTextWidth(L":") +
                        static_cast<FLOAT>(Scale(g_app.config.network_arrow_gap_dip, g_app.window.dpi)) +
                        MeasureTextWidth(L"99.9MB/s"));
            } else {
                column.width = std::max(column.width, MeasureTextWidth(column.width_sample));
            }
        }
        total_text_width += column.width;
    }

    const FLOAT available = std::max(1.0f, rect.right - rect.left);
    const FLOAT default_gap = static_cast<FLOAT>(Scale(g_app.config.column_gap_dip, g_app.window.dpi));
    FLOAT total_gap = 0.0f;
    for (size_t i = 0; i + 1 < columns.size(); ++i) {
        total_gap += columns[i].gap_after >= 0.0f ? columns[i].gap_after : default_gap;
    }
    FLOAT total_width = total_text_width + total_gap;

    if (total_width > available) {
        total_width = available;
    }

    const FLOAT row_height = (rect.bottom - rect.top) / 2.0f;
    FLOAT x = rect.left;
    for (const TextColumn& column : columns) {
        D2D1_RECT_F top_rect{x, rect.top, x + column.width, rect.top + row_height};
        D2D1_RECT_F bottom_rect{x, rect.top + row_height, x + column.width, rect.bottom};
        if (column.key_widget) {
            DrawKeyWidget(target, brush, top_rect, bottom_rect);
        } else if (column.split_prefix) {
            DrawSplitLine(target, brush, top_rect, column.top_prefix, column.top_value);
            DrawSplitLine(target, brush, bottom_rect, column.bottom_prefix, column.bottom_value);
        } else {
            DrawTextCell(target, brush, top_rect, column.top, DWRITE_TEXT_ALIGNMENT_LEADING);
            DrawTextCell(target, brush, bottom_rect, column.bottom, DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        const FLOAT gap_after = column.gap_after >= 0.0f ? column.gap_after : default_gap;
        x += column.width + gap_after;
        if (x > rect.left + total_width + default_gap) {
            break;
        }
    }
}

// Layered-window presentation and overlay frame lifecycle.
void ReportPresentFailure(
    const wchar_t* key,
    const wchar_t* source,
    const wchar_t* stage,
    DWORD error,
    int width,
    int height,
    bool error_available = true) {
    ++g_app.diagnostics.present_failures;
    LogErrorRateLimited(
        key,
        kFailureLogIntervalMs,
        L"event=present_failed source=%ls stage=%ls error_available=%d error=%lu width=%d height=%d failures=%u",
        source,
        stage,
        error_available ? 1 : 0,
        error,
        width,
        height,
        g_app.diagnostics.present_failures);
}

void MarkPresentSuccess(const wchar_t* source) {
    const DWORD success_tick = GetTickCount();
    g_app.diagnostics.last_present_success_tick = success_tick == 0 ? 1 : success_tick;
    ++g_app.diagnostics.present_success_sequence;
    const wchar_t* keys[] = {
        L"present.get_window_rect",
        L"present.invalid_input",
        L"present.get_screen_dc",
        L"present.create_memory_dc",
        L"present.create_bitmap",
        L"present.update_layered_window",
    };
    for (const wchar_t* key : keys) {
        LogFailureRecovered(
            key,
            L"event=component_recovered component=present source=%ls",
            source);
    }
}

bool PresentPixels(
    HWND hwnd,
    const BYTE* source_pixels,
    int width,
    int height,
    const wchar_t* source) {
    g_app.diagnostics.last_present_attempt_tick = GetTickCount();
    RECT window_rect{};
    SetLastError(ERROR_SUCCESS);
    if (!GetWindowRect(hwnd, &window_rect)) {
        const DWORD error = GetLastError();
        ReportPresentFailure(
            L"present.get_window_rect",
            source,
            L"get_window_rect",
            error,
            width,
            height,
            error != ERROR_SUCCESS);
        return false;
    }
    if (width <= 0 || height <= 0 || !source_pixels) {
        ReportPresentFailure(
            L"present.invalid_input",
            source,
            L"validate",
            ERROR_INVALID_PARAMETER,
            width,
            height);
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) {
        const DWORD error = GetLastError();
        ReportPresentFailure(
            L"present.get_screen_dc",
            source,
            L"get_screen_dc",
            error,
            width,
            height,
            error != ERROR_SUCCESS);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    SetLastError(ERROR_SUCCESS);
    HDC mem_dc = CreateCompatibleDC(screen_dc);
    if (!mem_dc) {
        const DWORD error = GetLastError();
        ReleaseDC(nullptr, screen_dc);
        ReportPresentFailure(
            L"present.create_memory_dc",
            source,
            L"create_memory_dc",
            error,
            width,
            height,
            error != ERROR_SUCCESS);
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    HBITMAP bitmap = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        const DWORD error = GetLastError();
        if (bitmap) {
            DeleteObject(bitmap);
        }
        DeleteDC(mem_dc);
        ReleaseDC(nullptr, screen_dc);
        ReportPresentFailure(
            L"present.create_bitmap",
            source,
            L"create_bitmap",
            error,
            width,
            height,
            error != ERROR_SUCCESS);
        return false;
    }

    std::memcpy(pixels, source_pixels, static_cast<size_t>(width) * height * 4);
    HGDIOBJ old_bitmap = SelectObject(mem_dc, bitmap);

    POINT dst{window_rect.left, window_rect.top};
    SIZE size{width, height};
    POINT src{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    SetLastError(ERROR_SUCCESS);
    const BOOL updated = UpdateLayeredWindow(hwnd, screen_dc, &dst, &size, mem_dc, &src, 0, &blend, ULW_ALPHA);
    const DWORD update_error = updated ? ERROR_SUCCESS : GetLastError();

    SelectObject(mem_dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(mem_dc);
    ReleaseDC(nullptr, screen_dc);
    if (!updated) {
        ReportPresentFailure(
            L"present.update_layered_window",
            source,
            L"update_layered_window",
            update_error,
            width,
            height,
            update_error != ERROR_SUCCESS);
    } else {
        MarkPresentSuccess(source);
    }
    return updated != FALSE;
}

void ResetLayeredSurface(HWND hwnd) {
    const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR clear_result = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style & ~WS_EX_LAYERED);
    DWORD style_error = clear_result == 0 ? GetLastError() : ERROR_SUCCESS;
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR restore_result = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style);
    if (restore_result == 0 && GetLastError() != ERROR_SUCCESS) {
        style_error = GetLastError();
    }
    if (style_error != ERROR_SUCCESS) {
        LogErrorRateLimited(
            L"overlay.reset_layered_style",
            kFailureLogIntervalMs,
            L"event=overlay_surface_reset_failed stage=style error=%lu",
            style_error);
    } else {
        LogFailureRecovered(
            L"overlay.reset_layered_style",
            L"event=component_recovered component=overlay_surface stage=style");
    }

    const BOOL positioned = SetWindowPos(
        hwnd,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    if (!positioned) {
        LogErrorRateLimited(
            L"overlay.reset_layered_position",
            kFailureLogIntervalMs,
            L"event=overlay_surface_reset_failed stage=set_window_pos error=%lu",
            GetLastError());
    } else {
        LogFailureRecovered(
            L"overlay.reset_layered_position",
            L"event=component_recovered component=overlay_surface stage=set_window_pos");
    }
}

bool CommitOverlayHidden(HWND hwnd, const wchar_t* trigger, const wchar_t* reason) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return true;
    }

    const BOOL hidden = SetWindowPos(
        hwnd,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
            SWP_NOACTIVATE | SWP_HIDEWINDOW);
    if (!hidden) {
        LogErrorRateLimited(
            L"overlay.hide_window",
            kFailureLogIntervalMs,
            L"event=overlay_visibility_commit result=failed desired=hidden trigger=%ls reason=%ls error=%lu",
            trigger,
            reason,
            GetLastError());
        return false;
    }

    LogFailureRecovered(
        L"overlay.hide_window",
        L"event=component_recovered component=overlay_visibility stage=hide");
    LogInfo(
        L"event=overlay_visibility_commit result=ok desired=hidden trigger=%ls reason=%ls generation=%llu present_sequence=%llu",
        trigger,
        reason,
        static_cast<unsigned long long>(g_app.diagnostics.overlay_generation),
        static_cast<unsigned long long>(g_app.diagnostics.present_success_sequence));
    return true;
}

bool PromoteVisibleOverlayTopmost(HWND hwnd, const wchar_t* trigger) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return false;
    }

    const BOOL positioned = SetWindowPos(
        hwnd,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (!positioned) {
        LogErrorRateLimited(
            L"overlay.screenshot_resume_topmost",
            kFailureLogIntervalMs,
            L"event=screenshot_resume_visibility result=failed trigger=%ls action=promote_topmost error=%lu",
            trigger,
            GetLastError());
        return false;
    }

    LogFailureRecovered(
        L"overlay.screenshot_resume_topmost",
        L"event=component_recovered component=screenshot_resume_visibility action=promote_topmost");
    return true;
}

bool PromoteVisibleOverlayForTaskView(HWND hwnd, const wchar_t* trigger) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return false;
    }

    const BOOL positioned = SetWindowPos(
        hwnd,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    const DWORD position_error = positioned ? ERROR_SUCCESS : GetLastError();
    HWND above = GetWindow(hwnd, GW_HWNDPREV);
    const std::wstring above_exe = WindowProcessBasename(above);
    const std::wstring above_class = WindowClassName(above);
    LogInfo(
        L"event=task_view_visibility result=%ls trigger=%ls action=promote_topmost "
        L"continuous_visible=%d owner=%p above=%p above_exe=%ls above_class=%ls above_visible=%d error=%lu",
        positioned ? L"ok" : L"failed",
        trigger,
        IsWindowVisible(hwnd) ? 1 : 0,
        GetWindow(hwnd, GW_OWNER),
        above,
        above_exe.c_str(),
        above_class.c_str(),
        above && IsWindowVisible(above) ? 1 : 0,
        position_error);
    return positioned != FALSE;
}

bool PrepareOverlayForShow(HWND hwnd, const wchar_t* trigger, bool reset_surface) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    if (reset_surface && IsWindowVisible(hwnd)) {
        if (!CommitOverlayHidden(hwnd, trigger, L"surface_reset")) {
            return false;
        }
    }

    if (!UpdateLayeredStyle(hwnd)) {
        return false;
    }
    RepositionWindow();
    if (reset_surface) {
        ResetLayeredSurface(hwnd);
    }

    const std::uint64_t required_after_sequence =
        g_app.diagnostics.present_success_sequence;
    RenderOverlay(hwnd);
    const bool presented = HasNewPresentation(
        g_app.diagnostics.present_success_sequence,
        required_after_sequence);
    if (!presented) {
        if (!IsWindowVisible(hwnd)) {
            CommitOverlayHidden(hwnd, trigger, L"present_failed_before_show");
        }
        LogErrorRateLimited(
            L"overlay.show_without_present",
            kFailureLogIntervalMs,
            L"event=overlay_visibility_commit result=blocked desired=visible trigger=%ls generation=%llu present_sequence=%llu required_after_sequence=%llu",
            trigger,
            static_cast<unsigned long long>(g_app.diagnostics.overlay_generation),
            static_cast<unsigned long long>(g_app.diagnostics.present_success_sequence),
            static_cast<unsigned long long>(required_after_sequence));
        return false;
    }

    const BOOL shown = SetWindowPos(
        hwnd,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (!shown || !IsWindowVisible(hwnd)) {
        LogError(
            L"event=overlay_visibility_commit result=failed desired=visible trigger=%ls error=%lu",
            trigger,
            shown ? ERROR_GEN_FAILURE : GetLastError());
        return false;
    }

    LogFailureRecovered(
        L"overlay.show_without_present",
        L"event=component_recovered component=overlay_visibility stage=present_before_show");
    LogInfo(
        L"event=overlay_visibility_commit result=ok desired=visible trigger=%ls generation=%llu present_sequence=%llu required_after_sequence=%llu",
        trigger,
        static_cast<unsigned long long>(g_app.diagnostics.overlay_generation),
        static_cast<unsigned long long>(g_app.diagnostics.present_success_sequence),
        static_cast<unsigned long long>(required_after_sequence));
    return true;
}

void UpdateFreezePolicy(bool screenshot_foreground) {
    if (screenshot_foreground == g_app.suppression.overlay_update_frozen) {
        return;
    }

    g_app.suppression.overlay_update_frozen = screenshot_foreground;
    if (screenshot_foreground) {
        g_app.suppression.refresh_resume_tick = 0;
        LogInfo(L"event=freeze_on reason=screenshot");
    } else {
        g_app.suppression.refresh_resume_tick = GetTickCount() + 800;
        LogInfo(L"event=freeze_off");
    }
}

bool UpdateSuppressionPolicy(
    const wchar_t* trigger,
    HWND foreground,
    bool screenshot_foreground) {
    const SuppressionPolicyState previous_state = g_app.suppression.policy;
    const SuppressionObservation observation =
        ObserveSuppression(
            foreground,
            screenshot_foreground,
            previous_state.committed);
    auto result = ReduceSuppressionPolicy(
        previous_state,
        observation,
        GetTickCount64(),
        kSuppressionPolicyConfig);
    g_app.suppression.policy = result.state;

    const bool candidate_started =
        result.state.candidate_active &&
        (!previous_state.candidate_active ||
         previous_state.candidate != result.state.candidate ||
         previous_state.candidate_profile != result.state.candidate_profile);
    if (candidate_started) {
        LogInfo(
            L"event=suppression_candidate trigger=%ls candidate=%ls committed=%ls profile=%ls required_delay_ms=%llu",
            trigger,
            SuppressionReasonName(result.state.candidate),
            SuppressionReasonName(result.state.committed),
            SuppressionTransitionProfileName(result.state.candidate_profile),
            static_cast<unsigned long long>(result.required_delay_ms));
    }

    if (!result.committed_changed) {
        return false;
    }

    ++g_app.suppression.decision_sequence;
    const DWORD now = GetTickCount();
    if (result.previous_committed == SuppressionReason::None &&
        result.state.committed != SuppressionReason::None) {
        g_app.suppression.suppression_started_tick = now == 0 ? 1 : now;
        LogInfo(
            L"event=suppression_on trigger=%ls reason=%ls decision=%llu dwell_ms=%llu profile=%ls required_delay_ms=%llu",
            trigger,
            SuppressionReasonName(result.state.committed),
            static_cast<unsigned long long>(g_app.suppression.decision_sequence),
            static_cast<unsigned long long>(result.candidate_dwell_ms),
            SuppressionTransitionProfileName(result.transition_profile),
            static_cast<unsigned long long>(result.required_delay_ms));
        LogSuppressionContext(result.state.committed);
    } else if (result.previous_committed != SuppressionReason::None &&
               result.state.committed == SuppressionReason::None) {
        const DWORD duration_ms = g_app.suppression.suppression_started_tick == 0
            ? 0
            : now - g_app.suppression.suppression_started_tick;
        g_app.suppression.suppression_started_tick = 0;
        LogInfo(
            L"event=suppression_off trigger=%ls previous_reason=%ls duration_ms=%lu decision=%llu dwell_ms=%llu profile=%ls required_delay_ms=%llu",
            trigger,
            SuppressionReasonName(result.previous_committed),
            duration_ms,
            static_cast<unsigned long long>(g_app.suppression.decision_sequence),
            static_cast<unsigned long long>(result.candidate_dwell_ms),
            SuppressionTransitionProfileName(result.transition_profile),
            static_cast<unsigned long long>(result.required_delay_ms));
        LogSuppressionContext(SuppressionReason::None);
    } else {
        LogInfo(
            L"event=suppression_changed trigger=%ls previous_reason=%ls reason=%ls decision=%llu dwell_ms=%llu profile=%ls required_delay_ms=%llu",
            trigger,
            SuppressionReasonName(result.previous_committed),
            SuppressionReasonName(result.state.committed),
            static_cast<unsigned long long>(g_app.suppression.decision_sequence),
            static_cast<unsigned long long>(result.candidate_dwell_ms),
            SuppressionTransitionProfileName(result.transition_profile),
            static_cast<unsigned long long>(result.required_delay_ms));
        LogSuppressionContext(result.state.committed);
    }
    return true;
}

void RefreshTrayIconForCommittedTaskbar() {
    HWND controller = g_app.window.controller_hwnd;
    if (!controller) {
        return;
    }
    RemoveTrayIcon(controller);
    if (HWND taskbar = CommittedTaskbarWindow(); taskbar && IsWindow(taskbar)) {
        AddTrayIcon(controller);
    }
}

void ReconcileOverlayStateOnce(const wchar_t* trigger, unsigned flags) {
    const unsigned incoming_flags = flags;
    flags |= g_app.reconcile.deferred_flags;
    g_app.reconcile.deferred_flags = OverlayReconcileNone;
    constexpr unsigned kDeferredActions =
        OverlayReconcileResetSurface |
        OverlayReconcileRefreshTrayIcon |
        OverlayReconcileApplyStyle;
    const auto defer_actions = [&](unsigned mask) {
        g_app.reconcile.deferred_flags |= flags & mask;
    };

    const auto taskbar_result = UpdateTaskbarIdentity(trigger);
    HWND committed_taskbar = CommittedTaskbarWindow();
    const bool committed_taskbar_ready =
        committed_taskbar &&
        IsWindow(committed_taskbar) &&
        IsTaskbarWindowClass(committed_taskbar) &&
        WindowProcessId(committed_taskbar) == CommittedTaskbarProcessId();

    if (taskbar_result.pending) {
        if (!committed_taskbar_ready) {
            CommitOverlayHidden(g_app.window.overlay_hwnd, trigger, L"taskbar_identity_pending");
        }
        defer_actions(kDeferredActions);
        return;
    }

    if (taskbar_result.committed_changed) {
        g_app.reconcile.recreate_pending = true;
        g_app.reconcile.next_destroy_retry_tick = 0;
    }
    if (g_app.reconcile.recreate_pending) {
        const DWORD now = GetTickCount();
        const bool old_overlay_still_exists =
            g_app.window.overlay_hwnd && IsWindow(g_app.window.overlay_hwnd);
        if (g_app.reconcile.next_destroy_retry_tick != 0 &&
            !TickPassed(now, g_app.reconcile.next_destroy_retry_tick) &&
            old_overlay_still_exists) {
            defer_actions(kDeferredActions);
            return;
        }
        CommitOverlayHidden(
            g_app.window.overlay_hwnd,
            trigger,
            L"taskbar_generation_change");
        if (!DestroyOverlayWindow(L"taskbar_generation_changed")) {
            g_app.reconcile.next_destroy_retry_tick = now + kOverlayRepairIntervalMs;
            defer_actions(kDeferredActions);
            return;
        }
        g_app.reconcile.recreate_pending = false;
        g_app.reconcile.next_destroy_retry_tick = 0;
        g_app.reconcile.next_visibility_retry_tick = 0;
        g_app.reconcile.visibility_retry_pending = false;
        flags |= OverlayReconcileRefreshTrayIcon;
    }
    if ((flags & OverlayReconcileRefreshTrayIcon) != 0) {
        RefreshTrayIconForCommittedTaskbar();
        flags &= ~OverlayReconcileRefreshTrayIcon;
    }

    if (!committed_taskbar_ready) {
        CommitOverlayHidden(g_app.window.overlay_hwnd, trigger, L"taskbar_unavailable");
        defer_actions(OverlayReconcileResetSurface | OverlayReconcileApplyStyle);
        return;
    }

    const std::uint64_t generation_before_ensure = g_app.diagnostics.overlay_generation;
    if (!EnsureOverlayWindow()) {
        defer_actions(OverlayReconcileResetSurface | OverlayReconcileApplyStyle);
        return;
    }
    HWND overlay = g_app.window.overlay_hwnd;
    const bool overlay_created =
        g_app.diagnostics.overlay_generation != generation_before_ensure;
    if (ReconcileInitialOwnerBinding(trigger)) {
        flags |= OverlayReconcileReposition | OverlayReconcileRender;
    }

    HWND foreground = GetForegroundWindow();
    const bool screenshot_foreground = IsBuiltinScreenshotForeground(foreground);
    const bool screenshot_was_frozen = g_app.suppression.overlay_update_frozen;
    UpdateFreezePolicy(screenshot_foreground);
    const bool screenshot_resumed =
        screenshot_was_frozen && !g_app.suppression.overlay_update_frozen;
    const bool suppression_changed =
        UpdateSuppressionPolicy(trigger, foreground, screenshot_foreground);
    const OverlayIntent intent = ComputeOverlayIntent(
        true,
        g_app.suppression.policy.committed,
        screenshot_foreground);

    if ((flags & OverlayReconcileApplyStyle) != 0) {
        if (!UpdateLayeredStyle(overlay)) {
            defer_actions(OverlayReconcileResetSurface | OverlayReconcileApplyStyle);
            return;
        }
        flags &= ~OverlayReconcileApplyStyle;
    }

    if (!intent.should_be_visible) {
        CommitOverlayHidden(
            overlay,
            trigger,
            SuppressionReasonName(g_app.suppression.policy.committed));
        defer_actions(OverlayReconcileResetSurface);
        return;
    }

    const bool resume_pending =
        g_app.suppression.refresh_resume_tick != 0 &&
        !TickPassed(GetTickCount(), g_app.suppression.refresh_resume_tick);
    if (intent.updates_frozen) {
        defer_actions(OverlayReconcileResetSurface);
        return;
    }
    if (ShouldPromoteOverlayDuringScreenshotResume(
            intent,
            screenshot_foreground,
            resume_pending)) {
        const bool promoted = PromoteVisibleOverlayTopmost(overlay, trigger);
        if (screenshot_resumed) {
            LogInfo(
                L"event=screenshot_resume_visibility result=%ls trigger=%ls action=promote_topmost continuous_visible=%d",
                promoted ? L"ok" : L"failed",
                trigger,
                IsWindowVisible(overlay) ? 1 : 0);
        }
        defer_actions(OverlayReconcileResetSurface);
        return;
    }
    if (g_app.suppression.refresh_resume_tick != 0) {
        flags |=
            OverlayReconcileSampleMetrics |
            OverlayReconcileReposition |
            OverlayReconcileRender;
        g_app.suppression.refresh_resume_tick = 0;
        PromoteVisibleOverlayTopmost(overlay, L"screenshot_resume_complete");
    }

    const bool restoring_visibility =
        suppression_changed &&
        g_app.suppression.policy.committed == SuppressionReason::None;
    const bool reset_surface = (flags & OverlayReconcileResetSurface) != 0;
    const bool visibility_missing = IsWindowVisible(overlay) == FALSE;
    const bool prepare_required =
        overlay_created ||
        taskbar_result.committed_changed ||
        restoring_visibility ||
        reset_surface ||
        visibility_missing ||
        g_app.reconcile.visibility_retry_pending;
    if (prepare_required) {
        const DWORD now = GetTickCount();
        const bool retry_ready =
            g_app.reconcile.next_visibility_retry_tick == 0 ||
            TickPassed(now, g_app.reconcile.next_visibility_retry_tick);
        const bool force_attempt =
            overlay_created ||
            taskbar_result.committed_changed ||
            restoring_visibility ||
            (incoming_flags & OverlayReconcileResetSurface) != 0;
        if (!force_attempt && !retry_ready) {
            defer_actions(OverlayReconcileResetSurface);
            return;
        }

        if (PrepareOverlayForShow(overlay, trigger, reset_surface)) {
            g_app.reconcile.next_visibility_retry_tick = 0;
            g_app.reconcile.visibility_retry_pending = false;
        } else {
            g_app.reconcile.next_visibility_retry_tick =
                now + kOverlayRepairIntervalMs;
            g_app.reconcile.visibility_retry_pending = true;
        }
        return;
    }

    if ((flags & OverlayReconcileSampleMetrics) != 0) {
        SampleMetrics();
    }
    bool should_render = (flags & OverlayReconcileRender) != 0;
    if ((flags & OverlayReconcileSampleKeys) != 0 &&
        g_app.config.show_key_widget &&
        SampleKeysIfChanged()) {
        should_render = true;
    }
    if (ShouldPromoteOverlayForTaskViewTransition(
            intent,
            IsWindowVisible(overlay) != FALSE,
            (flags & OverlayReconcilePromoteForTaskView) != 0)) {
        PromoteVisibleOverlayForTaskView(overlay, trigger);
    }
    if ((flags & OverlayReconcileReposition) != 0 && RepositionWindow()) {
        should_render = true;
    }
    if (should_render) {
        RenderOverlay(overlay);
    }

    RepairOverlayInvariant(trigger, CaptureOverlayState(true));
}

void ReconcileOverlayState(const wchar_t* trigger, unsigned flags) {
    g_app.reconcile.pending = true;
    g_app.reconcile.pending_flags |= flags;
    g_app.reconcile.pending_trigger = trigger ? trigger : L"unspecified";
    if (g_app.reconcile.active) {
        return;
    }

    g_app.reconcile.active = true;
    for (unsigned pass = 0; pass < 4 && g_app.reconcile.pending; ++pass) {
        const wchar_t* pending_trigger = g_app.reconcile.pending_trigger;
        const unsigned pending_flags = g_app.reconcile.pending_flags;
        g_app.reconcile.pending = false;
        g_app.reconcile.pending_flags = OverlayReconcileNone;
        ReconcileOverlayStateOnce(pending_trigger, pending_flags);
    }
    const bool needs_follow_up = g_app.reconcile.pending;
    g_app.reconcile.active = false;

    if (needs_follow_up) {
        RequestOverlayReconcile(L"reconcile_follow_up", OverlayReconcileNone);
    }
}

void RequestOverlayReconcile(const wchar_t* trigger, unsigned flags) {
    g_app.reconcile.queued_flags |= flags;
    g_app.reconcile.queued_trigger = trigger ? trigger : L"unspecified";
    if (g_app.reconcile.message_queued) {
        return;
    }

    HWND controller = g_app.window.controller_hwnd;
    if (!controller) {
        return;
    }
    g_app.reconcile.message_queued = true;
    if (!PostMessageW(controller, WM_RECONCILE, 0, 0)) {
        g_app.reconcile.message_queued = false;
        LogError(
            L"event=shell_message_failed message=reconcile error=%lu",
            GetLastError());
    }
}

void HandleQueuedOverlayReconcile() {
    g_app.reconcile.message_queued = false;
    std::wstring trigger = std::move(g_app.reconcile.queued_trigger);
    const unsigned flags = g_app.reconcile.queued_flags;
    g_app.reconcile.queued_trigger = L"queued";
    g_app.reconcile.queued_flags = OverlayReconcileNone;
    ReconcileOverlayState(trigger.c_str(), flags);
}

void RenderOverlay(HWND hwnd) {
    g_app.diagnostics.last_render_attempt_tick = GetTickCount();
    RECT client{};
    SetLastError(ERROR_SUCCESS);
    if (!GetClientRect(hwnd, &client)) {
        const DWORD error = GetLastError();
        ReportRenderFailure(
            L"render.get_client_rect",
            L"get_client_rect",
            error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error));
        return;
    }
    ReportRenderRecovered(L"render.get_client_rect", L"get_client_rect");

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        LogWarningRateLimited(
            L"render.invalid_client_size",
            kFailureLogIntervalMs,
            L"event=render_skipped reason=invalid_client_size width=%d height=%d dpi=%u",
            width,
            height,
            g_app.window.dpi);
        return;
    }
    LogFailureRecovered(
        L"render.invalid_client_size",
        L"event=component_recovered component=render stage=client_size");

    const HRESULT resources_result = EnsureRenderResources();
    if (FAILED(resources_result)) {
        return;
    }

    IWICBitmap* wic_bitmap = nullptr;
    ID2D1RenderTarget* target = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;

    HRESULT hr = g_app.render.resources.wic_factory->CreateBitmap(
        width,
        height,
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapCacheOnLoad,
        &wic_bitmap);
    if (FAILED(hr)) {
        ReportRenderFailure(L"render.create_bitmap", L"create_bitmap", hr, width, height);
    } else {
        ReportRenderRecovered(L"render.create_bitmap", L"create_bitmap");
    }
    if (SUCCEEDED(hr)) {
        D2D1_RENDER_TARGET_PROPERTIES props{};
        props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        props.dpiX = 96.0f;
        props.dpiY = 96.0f;
        props.usage = D2D1_RENDER_TARGET_USAGE_NONE;
        props.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;

        hr = g_app.render.resources.d2d_factory->CreateWicBitmapRenderTarget(wic_bitmap, &props, &target);
        if (FAILED(hr)) {
            ReportRenderFailure(L"render.create_target", L"create_target", hr, width, height);
        } else {
            ReportRenderRecovered(L"render.create_target", L"create_target");
        }
    }
    if (SUCCEEDED(hr)) {
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        D2D1_COLOR_F color{1.0f, 1.0f, 1.0f, 1.0f};
        hr = target->CreateSolidColorBrush(color, &brush);
        if (FAILED(hr)) {
            ReportRenderFailure(L"render.create_brush", L"create_brush", hr, width, height);
        } else {
            ReportRenderRecovered(L"render.create_brush", L"create_brush");
        }
    }

    const FLOAT pad_x = static_cast<FLOAT>(Scale(g_app.config.content_padding_x_dip, g_app.window.dpi));
    const FLOAT pad_y = static_cast<FLOAT>(Scale(5, g_app.window.dpi));
    D2D1_RECT_F content{
        pad_x,
        pad_y,
        static_cast<FLOAT>(width) - pad_x,
        static_cast<FLOAT>(height) - pad_y};
    const std::wstring disk_text = L"SSD: " + FormatPercent(g_app.metrics.current.disk);
    std::vector<TextColumn> columns{
        MakeNetworkColumn(
            FormatRate(g_app.metrics.current.up_bps),
            FormatRate(g_app.metrics.current.down_bps),
            static_cast<FLOAT>(Scale(g_app.config.gap_after_network_dip >= 0 ? g_app.config.gap_after_network_dip : g_app.config.column_gap_dip, g_app.window.dpi))),
        MakeTextColumn(
            L"CPU: " + FormatPercent(g_app.metrics.current.cpu),
            L"RAM: " + std::to_wstring(g_app.metrics.current.memory_load) + L"%",
            L"RAM: 100%",
            static_cast<FLOAT>(Scale(g_app.config.gap_after_system_dip >= 0 ? g_app.config.gap_after_system_dip : g_app.config.column_gap_dip, g_app.window.dpi))),
        MakeTextColumn(
            L"GPU: " + FormatPercent(g_app.metrics.current.gpu),
            disk_text,
            L"SSD: 100%",
            g_app.config.show_key_widget ? static_cast<FLOAT>(Scale(g_app.config.gap_after_disk_dip, g_app.window.dpi)) : -1.0f),
    };
    if (g_app.config.show_key_widget) {
        columns.push_back(MakeKeyWidgetColumn());
    }

    if (SUCCEEDED(hr)) {
        target->BeginDraw();
        D2D1_COLOR_F clear{0.0f, 0.0f, 0.0f, 0.0f};
        target->Clear(clear);
        DrawAdaptiveColumnsDwrite(
            target,
            brush,
            content,
            columns);
        hr = target->EndDraw();
        if (FAILED(hr)) {
            ReportRenderFailure(L"render.end_draw", L"end_draw", hr, width, height);
        } else {
            ReportRenderRecovered(L"render.end_draw", L"end_draw");
        }
    }

    if (SUCCEEDED(hr)) {
        WICRect rect{0, 0, width, height};
        std::vector<BYTE> frame(static_cast<size_t>(width) * height * 4);
        hr = wic_bitmap->CopyPixels(&rect, width * 4, static_cast<UINT>(frame.size()), frame.data());
        if (FAILED(hr)) {
            ReportRenderFailure(L"render.copy_pixels", L"copy_pixels", hr, width, height);
        } else {
            ReportRenderRecovered(L"render.copy_pixels", L"copy_pixels");
        }
        if (SUCCEEDED(hr)) {
            bool has_visible_pixels = false;
            for (size_t i = 3; i < frame.size(); i += 4) {
                if (frame[i] != 0) {
                    has_visible_pixels = true;
                    break;
                }
            }
            g_app.diagnostics.last_frame_has_visible_pixels = has_visible_pixels;
            if (!has_visible_pixels) {
                LogWarningRateLimited(
                    L"render.empty_frame",
                    kFailureLogIntervalMs,
                    L"event=render_empty_frame width=%d height=%d dpi=%u",
                    width,
                    height,
                    g_app.window.dpi);
            } else {
                LogFailureRecovered(
                    L"render.empty_frame",
                    L"event=render_pixels_recovered width=%d height=%d",
                    width,
                    height);
            }
            g_app.diagnostics.last_render_success_tick = GetTickCount();
        }
        if (SUCCEEDED(hr) && PresentPixels(hwnd, frame.data(), width, height, L"render")) {
            g_app.render.last_frame_width = width;
            g_app.render.last_frame_height = height;
        }
    }

    SafeRelease(brush);
    SafeRelease(target);
    SafeRelease(wic_bitmap);
}

void ValidatePaint(HWND hwnd) {
    PAINTSTRUCT ps{};
    BeginPaint(hwnd, &ps);
    EndPaint(hwnd, &ps);
}

LRESULT HandleTaskbarCreated() {
    LogInfo(L"event=taskbar_created");
    if (!g_app.window.monitor_initialized) {
        return 0;
    }

    ReconcileOverlayState(
        L"taskbar_created",
        OverlayReconcileReposition |
            OverlayReconcileRender |
            OverlayReconcileRefreshTrayIcon);
    return 0;
}

void InitializeMetricProviders() {
    g_app.metrics.initialization_pending = false;
    if (g_app.metrics.providers_initialized) {
        return;
    }

    g_app.metrics.providers_initialized = true;
    InitializePdhGroupMeasured(
        g_app.metrics.gpu,
        L"gpu",
        L"\\GPU Engine(*)\\Utilization Percentage",
        true,
        L"initial");
    InitializePdhGroupMeasured(
        g_app.metrics.disk,
        L"disk",
        L"\\PhysicalDisk(_Total)\\% Disk Time",
        false,
        L"initial");
    SampleMetrics();
    ReconcileOverlayState(L"metric_provider_init", OverlayReconcileRender);
    RecordStartupPerformance(L"monitor_ready");
}

void RequestMetricProviderInitialization(HWND controller) {
    if (g_app.metrics.providers_initialized || g_app.metrics.initialization_pending) {
        return;
    }

    g_app.metrics.initialization_pending = true;
    if (!PostMessageW(controller, WM_INITIALIZE_METRICS, 0, 0)) {
        LogWarning(
            L"event=metric_initialization_defer_failed error=%lu",
            GetLastError());
        InitializeMetricProviders();
    }
}

void InitializeMonitor(HWND controller) {
    if (g_app.window.monitor_initialized) {
        return;
    }

    g_app.window.monitor_initialized = true;
    LogInfo(L"event=monitor_init delayed=%d", g_app.window.launched_at_startup ? 1 : 0);
    g_app.placement.taskbar_identity.committed = CurrentTaskbarIdentity();
    g_app.placement.taskbar_identity.candidate =
        g_app.placement.taskbar_identity.committed;
    AddTrayIcon(controller);
    RegisterTrayEventHooks();
    SampleMetrics();
    const bool refresh_timer = InstallTimer(controller, kRefreshTimer, 1000, L"refresh");
    const bool placement_timer = SetPlacementTimer(kPlacementIntervalMs);
    const bool state_timer = InstallTimer(controller, kStateTimer, kStateIntervalMs, L"state");
    LogInfo(
        L"event=timers_initialized refresh=%d placement=%d state=%d",
        refresh_timer ? 1 : 0,
        placement_timer ? 1 : 0,
        state_timer ? 1 : 0);
    ReconcileOverlayState(
        L"monitor_init",
        OverlayReconcileReposition | OverlayReconcileRender);
    if (g_app.window.overlay_hwnd) {
        UpdateWindow(g_app.window.overlay_hwnd);
    }
    RecordStartupPerformance(L"overlay_ready");
    RequestMetricProviderInitialization(controller);
}

LRESULT HandleControllerCreate(HWND hwnd) {
    g_app.window.controller_hwnd = hwnd;
    RecordStartupPerformance(L"controller_ready");
    if (g_app.window.launched_at_startup) {
        if (!InstallTimer(hwnd, kStartupInitTimer, kStartupInitDelayMs, L"startup_init")) {
            LogWarning(L"event=startup_delay_bypassed reason=timer_install_failed");
            InitializeMonitor(hwnd);
        }
    } else {
        InitializeMonitor(hwnd);
    }
    return 0;
}

LRESULT HandleTimer(HWND hwnd, UINT_PTR timer_id) {
    if (timer_id == kStartupInitTimer) {
        KillTimer(hwnd, kStartupInitTimer);
        InitializeMonitor(hwnd);
        return 0;
    }

    if (timer_id == kStateTimer) {
        const DWORD now = GetTickCount();
        if (g_app.diagnostics.last_state_timer_tick != 0) {
            g_app.diagnostics.last_state_timer_gap_ms = now - g_app.diagnostics.last_state_timer_tick;
            if (g_app.diagnostics.last_state_timer_gap_ms > 2000) {
                LogWarningRateLimited(
                    L"timer.state_gap",
                    kFailureLogIntervalMs,
                    L"event=timer_gap timer=state gap_ms=%lu",
                    g_app.diagnostics.last_state_timer_gap_ms);
            } else {
                LogFailureRecovered(
                    L"timer.state_gap",
                    L"event=component_recovered component=timer timer=state");
            }
        }
        g_app.diagnostics.last_state_timer_tick = now;
        const std::uint64_t now_ms = GetTickCount64();
        unsigned flags = OverlayReconcileSampleKeys;
        const wchar_t* trigger = L"state_timer";
        if (ShouldRunTaskViewStabilization(
                g_app.reconcile.task_view_stabilize_pending,
                now_ms,
                g_app.reconcile.task_view_stabilize_after_ms)) {
            g_app.reconcile.task_view_stabilize_pending = false;
            g_app.reconcile.task_view_stabilize_after_ms = 0;
            flags |= OverlayReconcileReposition | OverlayReconcilePromoteForTaskView;
            trigger = L"task_view_stabilize";
        }
        ReconcileOverlayState(trigger, flags);
        MaybeLogHealth(L"state_timer");
        return 0;
    }

    const wchar_t* trigger = timer_id == kRefreshTimer ? L"refresh_timer" : L"placement_timer";
    const unsigned flags = timer_id == kRefreshTimer
        ? OverlayReconcileSampleMetrics | OverlayReconcileRender
        : OverlayReconcileReposition;
    ReconcileOverlayState(trigger, flags);
    MaybeLogHealth(trigger);
    if (timer_id == kRefreshTimer) {
        MaybeRecordStartupPerformanceSettled();
    }
    return 0;
}

LRESULT HandleDpiChanged(HWND, WPARAM wparam, LPARAM lparam) {
    const UINT previous_dpi = g_app.window.dpi;
    g_app.window.dpi = HIWORD(wparam);
    LogInfo(
        L"event=dpi_changed old_dpi=%u new_dpi=%u has_suggested_rect=%d",
        previous_dpi,
        g_app.window.dpi,
        lparam ? 1 : 0);
    ReconcileOverlayState(
        L"dpi_change",
        OverlayReconcileReposition | OverlayReconcileRender);
    return 0;
}

LRESULT HandleDisplayChange(const wchar_t* trigger) {
    if (!g_app.window.monitor_initialized) {
        return 0;
    }

    RequestOverlayReconcile(
        trigger,
        OverlayReconcileReposition | OverlayReconcileRender);
    return 0;
}

LRESULT HandleSettingChange(LPARAM lparam) {
    if (lparam && std::wcscmp(reinterpret_cast<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0) {
        EnableSystemMenuTheme();
    }
    return HandleDisplayChange(L"setting_change");
}

LRESULT HandleTrayLayoutChanged() {
    InterlockedExchange(&g_app.placement.tray_layout_update_pending, 0);
    if (!g_app.window.monitor_initialized) {
        return 0;
    }

    LogInfo(L"event=tray_layout_changed");
    unsigned flags = OverlayReconcileReposition;
    if (IsTaskViewWindow(GetForegroundWindow())) {
        flags |= OverlayReconcilePromoteForTaskView;
    }
    RequestOverlayReconcile(L"tray_layout_change", flags);
    return 0;
}

LRESULT HandleForegroundChanged(WPARAM source_event, LPARAM source_lparam) {
    if (!g_app.window.monitor_initialized) {
        return 0;
    }

    const HWND source = reinterpret_cast<HWND>(source_lparam);
    const std::wstring source_exe = WindowProcessBasename(source);
    const std::wstring source_class = WindowClassName(source);
    const bool task_view =
        source_exe == L"explorer.exe" &&
        source_class == L"XamlExplorerHostIslandWindow";
    LogInfo(
        L"event=foreground_changed source_event=%llu source_exe=%ls source_class=%ls task_view=%d overlay_visible=%d",
        static_cast<unsigned long long>(source_event),
        source_exe.c_str(),
        source_class.c_str(),
        task_view ? 1 : 0,
        IsWindowVisible(g_app.window.overlay_hwnd) ? 1 : 0);
    const bool transition_completed = ShouldCompleteTaskViewTransition(
        task_view,
        g_app.reconcile.task_view_transition_pending);
    if (!task_view && !transition_completed) {
        return 0;
    }
    g_app.reconcile.task_view_transition_pending = task_view;
    if (task_view) {
        g_app.reconcile.task_view_stabilize_pending = false;
        g_app.reconcile.task_view_stabilize_after_ms = 0;
    } else {
        g_app.reconcile.task_view_stabilize_pending = true;
        g_app.reconcile.task_view_stabilize_after_ms =
            GetTickCount64() + kTaskViewStabilizeDelayMs;
    }
    RequestOverlayReconcile(
        task_view ? L"task_view_started" : L"task_view_completed",
        OverlayReconcileReposition | OverlayReconcilePromoteForTaskView);
    return 0;
}

LRESULT HandleTrayIcon(HWND hwnd, LPARAM lparam) {
    if (LOWORD(lparam) == WM_CONTEXTMENU) {
        ShowTrayMenu(hwnd);
    } else if (LOWORD(lparam) == WM_LBUTTONDBLCLK) {
        ReconcileOverlayState(L"tray_double_click", OverlayReconcileReposition);
    }
    return 0;
}

LRESULT HandleControllerDestroy(HWND hwnd) {
    MaybeLogHealth(L"shutdown", true);
    g_app.window.monitor_initialized = false;
    KillTimer(hwnd, kRefreshTimer);
    KillTimer(hwnd, kPlacementTimer);
    KillTimer(hwnd, kStateTimer);
    KillTimer(hwnd, kStartupInitTimer);
    UnregisterTrayEventHooks();
    RemoveTrayIcon(hwnd);
    DestroyOverlayWindow(L"controller_shutdown");
    g_app.window.controller_hwnd = nullptr;
    if (g_app.metrics.gpu.query) {
        PdhCloseQuery(g_app.metrics.gpu.query);
        g_app.metrics.gpu.query = nullptr;
    }
    if (g_app.metrics.disk.query) {
        PdhCloseQuery(g_app.metrics.disk.query);
        g_app.metrics.disk.query = nullptr;
    }
    ReleaseRenderResources();
    simple_monitor::FlushLogSummaries();
    LogInfo(
        L"event=shutdown build=dev trigger=%ls",
        g_app.window.shutdown_trigger ? g_app.window.shutdown_trigger : L"unknown");
    PostQuitMessage(0);
    return 0;
}

LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_PAINT:
        ValidatePaint(hwnd);
        return 0;

    case WM_DPICHANGED:
        return HandleDpiChanged(hwnd, wparam, lparam);

    case WM_DESTROY:
        if (g_app.window.overlay_hwnd == hwnd) {
            const bool expected = g_app.window.overlay_destroy_expected;
            if (!expected) {
                LogOverlayWindowState(L"overlay_destroy_unexpected_context", hwnd);
            }
            LogInfo(L"event=overlay_destroyed hwnd=%p expected=%d", hwnd, expected ? 1 : 0);
            g_app.window.overlay_hwnd = nullptr;
            g_app.placement.taskbar_owner = nullptr;
            ResetInitialOwnerBindingState();
            ResetOverlayPresentDiagnostics();
            if (!expected && g_app.window.monitor_initialized && g_app.window.controller_hwnd) {
                RequestOverlayReconcile(
                    L"overlay_destroyed_unexpected",
                    OverlayReconcileReposition | OverlayReconcileRender);
            }
        }
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// The hidden controller owns process lifetime; the overlay persists for one
// committed taskbar generation.
LRESULT CALLBACK ControllerWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (g_app.window.taskbar_created != 0 && msg == g_app.window.taskbar_created) {
        return HandleTaskbarCreated();
    }

    switch (msg) {
    case WM_CREATE:
        return HandleControllerCreate(hwnd);

    case WM_TIMER:
        return HandleTimer(hwnd, static_cast<UINT_PTR>(wparam));

    case WM_DISPLAYCHANGE:
        LogInfo(L"event=environment_changed type=display");
        return HandleDisplayChange(L"display_change");

    case WM_DEVICECHANGE:
        LogInfo(
            L"event=environment_changed type=device code=0x%llx payload=0x%llx",
            static_cast<unsigned long long>(wparam),
            static_cast<unsigned long long>(lparam));
        return HandleDisplayChange(L"device_change");

    case WM_SETTINGCHANGE:
        return HandleSettingChange(lparam);

    case WM_COMMAND:
        HandleMenuCommand(hwnd, LOWORD(wparam));
        return 0;

    case WM_TRAY_LAYOUT_CHANGED:
        return HandleTrayLayoutChanged();

    case WM_FOREGROUND_CHANGED:
        return HandleForegroundChanged(wparam, lparam);

    case WM_RECONCILE:
        HandleQueuedOverlayReconcile();
        MaybeLogHealth(L"reconcile_message", true);
        return 0;

    case WM_INITIALIZE_METRICS:
        InitializeMetricProviders();
        return 0;

    case WM_TRAYICON:
        return HandleTrayIcon(hwnd, lparam);

    case WM_DESTROY:
        return HandleControllerDestroy(hwnd);
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool RegisterWindowClasses(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ControllerWindowProc;
    wc.hInstance = instance;
    wc.hIcon = LoadAppIcon(instance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hIconSm = LoadAppIcon(instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kControllerWindowClass;
    if (RegisterClassExW(&wc) == 0) {
        LogError(
            L"event=startup_component_failed component=window_class stage=controller error=%lu",
            GetLastError());
        return false;
    }

    wc.lpfnWndProc = OverlayWindowProc;
    wc.lpszClassName = kOverlayWindowClass;
    if (RegisterClassExW(&wc) == 0) {
        LogError(
            L"event=startup_component_failed component=window_class stage=overlay error=%lu",
            GetLastError());
        return false;
    }
    return true;
}

int Run(HINSTANCE instance) {
    g_app.startup_performance.process_entry_tick = GetTickCount64();
    g_app.window.instance = instance;
    g_app.window.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    const DWORD taskbar_message_error =
        g_app.window.taskbar_created == 0 ? GetLastError() : ERROR_SUCCESS;
    g_app.window.launched_at_startup = CommandLineHasFlag(L"--startup");
    EnableSystemMenuTheme();
    const HRESULT co_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_app.window.com_initialized = SUCCEEDED(co_result);
    LoadAppConfig();
    simple_monitor::ResetLog();
    LogInfo(
        L"event=startup schema=2 build=dev launch=%ls taskbar_message_id=%u com_hresult=0x%08lx",
        g_app.window.launched_at_startup ? L"startup" : L"manual",
        g_app.window.taskbar_created,
        static_cast<unsigned long>(co_result));
    LogConfigSnapshot(L"config_loaded");
    RecordStartupPerformance(L"logging_ready");
    if (taskbar_message_error != ERROR_SUCCESS) {
        LogError(
            L"event=startup_component_failed component=taskbar_message error=%lu",
            taskbar_message_error);
    }
    if (FAILED(co_result)) {
        LogWarning(
            L"event=startup_component_failed component=com hresult=0x%08lx",
            static_cast<unsigned long>(co_result));
    }

    using DpiAwarenessContext = HANDLE;
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DpiAwarenessContext);
    auto set_dpi_awareness =
        simple_monitor::LoadOptionalFunction<SetProcessDpiAwarenessContextFn>(
            GetModuleHandleW(L"user32.dll"),
            "SetProcessDpiAwarenessContext");
    if (set_dpi_awareness) {
        if (!set_dpi_awareness(
                reinterpret_cast<DpiAwarenessContext>(static_cast<INT_PTR>(-4)))) {
            const DWORD error = GetLastError();
            if (error == ERROR_ACCESS_DENIED) {
                LogDebug(
                    L"event=startup_component_state component=dpi_awareness "
                    L"state=already_configured error=%lu",
                    static_cast<unsigned long>(error));
            } else {
                LogWarning(
                    L"event=startup_component_failed component=dpi_awareness error=%lu",
                    static_cast<unsigned long>(error));
            }
        }
    } else {
        LogDebug(L"event=startup_component_unavailable component=dpi_awareness_api");
    }

    if (!RegisterWindowClasses(instance)) {
        LogError(L"event=startup_failed stage=register_window_classes");
        if (g_app.window.com_initialized) {
            CoUninitialize();
        }
        return 1;
    }

    HWND controller = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kControllerWindowClass,
        L"Simple Monitor Controller",
        WS_POPUP,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!controller) {
        LogError(
            L"event=startup_failed stage=create_controller error=%lu",
            GetLastError());
        if (g_app.window.com_initialized) {
            CoUninitialize();
        }
        return 1;
    }
    MSG msg{};
    BOOL message_result = 0;
    while ((message_result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (message_result == -1) {
        LogError(L"event=message_loop_failed error=%lu", GetLastError());
    }

    if (g_app.window.com_initialized) {
        CoUninitialize();
    }

    return message_result == -1 ? 1 : static_cast<int>(msg.wParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    return Run(instance);
}
