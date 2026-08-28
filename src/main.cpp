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
#include <objbase.h>
#include <oleacc.h>

#include "app_support.h"
#include "logging.h"
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
constexpr wchar_t kWindowClass[] = L"SimpleMonitorOverlayWindow";
constexpr wchar_t kRunValue[] = L"SimpleMonitor";

// Timer roles:
// - refresh: sample metrics and repaint the overlay.
// - placement: keep the overlay aligned to the taskbar tray anchor.
// - state: react quickly to fullscreen/screenshot/key-state changes.
// - z-order burst: briefly fight foreground and Shell z-order animations.
// - startup init: defer expensive monitor setup when launched at logon.
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT_PTR kPlacementTimer = 2;
constexpr UINT_PTR kStateTimer = 3;
constexpr UINT_PTR kZOrderBurstTimer = 4;
constexpr UINT_PTR kStartupInitTimer = 5;
constexpr UINT kStateIntervalMs = 100;
constexpr UINT kPlacementIntervalMs = 5000;
constexpr UINT kZOrderBurstIntervalMs = 16;
constexpr UINT kStartupInitDelayMs = 8000;
constexpr DWORD kGpuGroupRefreshIntervalMs = 5000;
constexpr DWORD kStartupWarmupMs = 15000;
constexpr DWORD kZOrderBurstDurationMs = 1500;
constexpr unsigned kFailureLogIntervalMs = 30000;
constexpr UINT kTrayIconId = 1;
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_TRAY_LAYOUT_CHANGED = WM_APP + 2;
constexpr UINT WM_FOREGROUND_CHANGED = WM_APP + 3;
constexpr int kAppIconResource = 101;
constexpr bool kAttachOverlayToTaskbarOwner = false;

enum MenuId : UINT {
    ID_CLICK_THROUGH = 1001,
    ID_STARTUP = 1002,
    ID_EXIT = 1003,
    ID_OPEN_CONFIG = 1004,
    ID_RELOAD_CONFIG = 1005,
    ID_RECOVER_TASKBAR = 1006,
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
    double down_bps = 0.0;
    double up_bps = 0.0;
};

struct PdhGroup {
    HQUERY query = nullptr;
    std::vector<HCOUNTER> counters;
    bool ready = false;
    bool needs_second_sample = false;
    double value = -1.0;
    std::wstring wildcard_path;
    DWORD last_refresh_tick = 0;
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
    HWND hwnd = nullptr;
    UINT taskbar_created = 0;
    UINT dpi = 96;
    const wchar_t* shutdown_trigger = L"window_destroy";
    bool com_initialized = false;
    bool launched_at_startup = false;
    bool monitor_initialized = false;
};

struct TrayState {
    bool click_through = false;
    std::vector<HWINEVENTHOOK> event_hooks;
};

struct PlacementState {
    HWND taskbar_owner = nullptr;
    bool maintaining_z_order = false;
    bool timer_fast = false;
    LONG tray_layout_update_pending = 0;
    RECT last_logged_taskbar_rect{};
    RECT last_logged_anchor_rect{};
    RECT last_logged_overlay_rect{};
    bool has_last_logged_taskbar_rect = false;
    bool has_last_logged_anchor_rect = false;
    bool has_last_logged_overlay_rect = false;
    int last_logged_anchor_mode = -1;
    bool control_center_was_foreground = false;
    DWORD z_order_burst_until = 0;
};

struct SuppressionState {
    bool overlay_update_frozen = false;
    bool overlay_suppressed = false;
    DWORD startup_warmup_until = 0;
    DWORD refresh_resume_tick = 0;
};

struct MetricsState {
    CpuSampler cpu;
    NetworkSampler network;
    PdhGroup gpu;
    PdhGroup disk;
    Metrics current;
};

struct RenderState {
    RenderResources resources;
    std::vector<BYTE> last_frame;
    int last_frame_width = 0;
    int last_frame_height = 0;
};

struct AppState {
    WindowState window;
    TrayState tray;
    PlacementState placement;
    SuppressionState suppression;
    MetricsState metrics;
    RenderState render;
    simple_monitor::Config config;
};

AppState g_app;

using simple_monitor::ConfigPath;
using simple_monitor::LogDebug;
using simple_monitor::LogError;
using simple_monitor::LogErrorRateLimited;
using simple_monitor::LogFailureRecovered;
using simple_monitor::LogInfo;
using simple_monitor::LogWarning;
using simple_monitor::LogWarningRateLimited;
using simple_monitor::ModuleDir;

// Forward declarations for cross-section entry points.
void RenderOverlay(HWND hwnd);
bool UpdateOverlaySuppression(HWND hwnd);
void ResetLayeredSurface(HWND hwnd);

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
        simple_monitor::DebugLogPath(),
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
        L"event=%ls path=\"%ls\" exists=%d debug_log=%d log_level=%ls "
        L"content_padding_x=%d column_gap=%d "
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

// Startup integration.
bool IsStartupEnabled() {
    return simple_monitor::IsStartupEnabled(kRunValue);
}

bool SetStartupEnabled(bool enabled) {
    const bool succeeded = simple_monitor::SetStartupEnabled(kRunValue, enabled);
    if (!succeeded) {
        LogError(L"event=startup_setting_failed enabled=%d", enabled ? 1 : 0);
        return false;
    }
    LogInfo(L"event=startup_setting_changed enabled=%d result=ok", enabled ? 1 : 0);
    return true;
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

bool IsShellPopupOrDesktopWindow(HWND hwnd) {
    if (!hwnd) {
        return false;
    }

    return WindowClassIs(hwnd, L"#32768") ||
           WindowClassIs(hwnd, L"Progman") ||
           WindowClassIs(hwnd, L"WorkerW");
}

bool IsTaskbarWindowClass(HWND hwnd) {
    return WindowClassIs(hwnd, L"Shell_TrayWnd") ||
           WindowClassIs(hwnd, L"Shell_SecondaryTrayWnd");
}

bool IsBuiltinScreenshotForeground() {
    HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == g_app.window.hwnd) {
        return false;
    }

    const std::wstring exe = WindowProcessBasename(foreground);
    return exe == L"snippingtool.exe" ||
           exe == L"screenclippinghost.exe" ||
           exe == L"snipandsketch.exe";
}

bool ShouldFreezeOverlayUpdate() {
    return IsBuiltinScreenshotForeground();
}

HWND TaskbarWindow() {
    return FindWindowW(L"Shell_TrayWnd", nullptr);
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

const wchar_t* SuppressedNotificationStateReason() {
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    if (FAILED(SHQueryUserNotificationState(&state))) {
        return nullptr;
    }

    if (state == QUNS_RUNNING_D3D_FULL_SCREEN) {
        return L"d3d_fullscreen";
    }
    if (state == QUNS_PRESENTATION_MODE) {
        return L"presentation";
    }
    return nullptr;
}

bool IsIgnoredShellWindow(HWND hwnd) {
    return !hwnd ||
           hwnd == g_app.window.hwnd ||
           IsTaskbarRelatedWindow(hwnd) ||
           IsShellPopupOrDesktopWindow(hwnd) ||
           WindowProcessBasename(hwnd) == L"explorer.exe";
}

bool IsEligibleFullscreenAppWindow(HWND hwnd) {
    return hwnd &&
           !IsIgnoredShellWindow(hwnd) &&
           IsWindowVisible(hwnd) &&
           !IsIconic(hwnd);
}

bool WindowCoversMonitor(HWND window) {
    RECT window_rect{};
    if (!GetWindowRect(window, &window_rect)) {
        return false;
    }

    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!monitor) {
        return false;
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) {
        return false;
    }

    const int tolerance = Scale(2, WindowDpi(g_app.window.hwnd ? g_app.window.hwnd : window));
    return std::abs(window_rect.left - mi.rcMonitor.left) <= tolerance &&
           std::abs(window_rect.top - mi.rcMonitor.top) <= tolerance &&
           std::abs(window_rect.right - mi.rcMonitor.right) <= tolerance &&
           std::abs(window_rect.bottom - mi.rcMonitor.bottom) <= tolerance;
}

bool IsFullscreenForegroundWindow() {
    HWND foreground = GetForegroundWindow();
    if (IsIgnoredShellWindow(foreground)) {
        return false;
    }

    DWORD foreground_process_id = 0;
    GetWindowThreadProcessId(foreground, &foreground_process_id);
    HWND candidate = GetAncestor(foreground, GA_ROOT);
    for (unsigned depth = 0; candidate && depth < 8; ++depth) {
        DWORD candidate_process_id = 0;
        GetWindowThreadProcessId(candidate, &candidate_process_id);
        if (candidate_process_id == foreground_process_id &&
            IsEligibleFullscreenAppWindow(candidate) &&
            WindowCoversMonitor(candidate)) {
            return true;
        }

        HWND owner = GetWindow(candidate, GW_OWNER);
        HWND next = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
        if (next == candidate) {
            break;
        }
        candidate = next;
    }
    return false;
}

bool StartupWarmupActive() {
    return g_app.suppression.startup_warmup_until != 0 &&
           static_cast<LONG>(GetTickCount() - g_app.suppression.startup_warmup_until) < 0;
}

const wchar_t* OverlaySuppressionReason() {
    if (const wchar_t* state_reason = SuppressedNotificationStateReason()) {
        return state_reason;
    }
    if (IsFullscreenForegroundWindow()) {
        if (StartupWarmupActive()) {
            return nullptr;
        }
        return L"fullscreen_window";
    }
    return nullptr;
}

void LogFullscreenSuppressionContext(const wchar_t* transition) {
    if (!simple_monitor::IsLogEnabled(simple_monitor::LogLevel::Info)) {
        return;
    }

    HWND foreground = GetForegroundWindow();
    HWND root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;

    RECT foreground_rect{};
    RECT root_rect{};
    RECT monitor_rect{};
    GetWindowRect(foreground, &foreground_rect);
    GetWindowRect(root, &root_rect);

    HMONITOR monitor = root ? MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (monitor) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(monitor, &mi)) {
            monitor_rect = mi.rcMonitor;
        }
    }

    const std::wstring foreground_exe = WindowProcessBasename(foreground);
    const std::wstring root_exe = WindowProcessBasename(root);
    const std::wstring foreground_class = WindowClassName(foreground);
    const std::wstring root_class = WindowClassName(root);

    LogInfo(
        L"event=fullscreen_context transition=%ls foreground_hwnd=%p foreground_exe=%ls foreground_class=%ls foreground_rect=(%ld,%ld,%ld,%ld) "
        L"root_hwnd=%p root_exe=%ls root_class=%ls root_rect=(%ld,%ld,%ld,%ld) monitor_rect=(%ld,%ld,%ld,%ld)",
        transition,
        foreground,
        foreground_exe.c_str(),
        foreground_class.c_str(),
        foreground_rect.left,
        foreground_rect.top,
        foreground_rect.right,
        foreground_rect.bottom,
        root,
        root_exe.c_str(),
        root_class.c_str(),
        root_rect.left,
        root_rect.top,
        root_rect.right,
        root_rect.bottom,
        monitor_rect.left,
        monitor_rect.top,
        monitor_rect.right,
        monitor_rect.bottom);
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
        return;
    }

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
    if (GetIfTable2(&table) != NO_ERROR || table == nullptr) {
        return;
    }

    const DWORD now = GetTickCount();
    const DWORD elapsed_ms = sampler.has_sample ? now - sampler.tick : 0;
    const double seconds = elapsed_ms > 0 ? elapsed_ms / 1000.0 : 0.0;
    double down_bps = 0.0;
    double up_bps = 0.0;

    for (NetworkSampler::InterfaceSample& sample : sampler.interfaces) {
        sample.seen = false;
    }

    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (row.OperStatus != IfOperStatusUp || row.Type == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }

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
}

void ResetPdhGroup(PdhGroup& group) {
    if (group.query) {
        PdhCloseQuery(group.query);
        group.query = nullptr;
    }
    group.counters.clear();
    group.ready = false;
    group.needs_second_sample = false;
    group.last_refresh_tick = 0;
}

void InitPdhGroup(PdhGroup& group, const wchar_t* wildcard_path) {
    group.wildcard_path = wildcard_path;
    ResetPdhGroup(group);
    if (PdhOpenQueryW(nullptr, 0, &group.query) != ERROR_SUCCESS) {
        return;
    }

    DWORD buffer_size = 0;
    PDH_STATUS expand_result = PdhExpandWildCardPathW(
        nullptr,
        wildcard_path,
        nullptr,
        &buffer_size,
        0);

    if (expand_result != static_cast<PDH_STATUS>(PDH_MORE_DATA) || buffer_size == 0) {
        PdhCloseQuery(group.query);
        group.query = nullptr;
        return;
    }

    std::vector<wchar_t> buffer(buffer_size);
    expand_result = PdhExpandWildCardPathW(
        nullptr,
        wildcard_path,
        buffer.data(),
        &buffer_size,
        0);

    if (expand_result != ERROR_SUCCESS) {
        PdhCloseQuery(group.query);
        group.query = nullptr;
        return;
    }

    const wchar_t* cursor = buffer.data();
    while (*cursor != L'\0') {
        HCOUNTER counter = nullptr;
        if (PdhAddCounterW(group.query, cursor, 0, &counter) == ERROR_SUCCESS) {
            group.counters.push_back(counter);
        }
        cursor += std::wcslen(cursor) + 1;
    }

    if (group.counters.empty()) {
        PdhCloseQuery(group.query);
        group.query = nullptr;
        return;
    }

    PdhCollectQueryData(group.query);
    group.ready = true;
    group.needs_second_sample = true;
    group.last_refresh_tick = GetTickCount();
}

void RebuildPdhGroup(PdhGroup& group) {
    if (group.wildcard_path.empty()) {
        return;
    }

    const std::wstring wildcard_path = group.wildcard_path;
    InitPdhGroup(group, wildcard_path.c_str());
}

void RefreshPdhGroupIfDue(PdhGroup& group) {
    if (group.wildcard_path.empty()) {
        return;
    }

    if (!group.ready || group.query == nullptr) {
        RebuildPdhGroup(group);
        return;
    }

    const DWORD now = GetTickCount();
    if (TickPassed(now, group.last_refresh_tick + kGpuGroupRefreshIntervalMs)) {
        RebuildPdhGroup(group);
    }
}

double SamplePdhGroup(PdhGroup& group, bool sum_values) {
    if (!group.ready || group.query == nullptr) {
        return -1.0;
    }

    if (PdhCollectQueryData(group.query) != ERROR_SUCCESS) {
        return -1.0;
    }

    if (group.needs_second_sample) {
        group.needs_second_sample = false;
        return group.value;
    }

    double total = 0.0;
    double max_value = -1.0;
    bool any = false;

    for (HCOUNTER counter : group.counters) {
        PDH_FMT_COUNTERVALUE value{};
        if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS ||
            value.CStatus != ERROR_SUCCESS) {
            continue;
        }

        any = true;
        total += value.doubleValue;
        max_value = std::max(max_value, value.doubleValue);
    }

    if (!any) {
        return -1.0;
    }

    group.value = ClampPercent(sum_values ? total : max_value);
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
    RefreshPdhGroupIfDue(g_app.metrics.gpu);
    g_app.metrics.current.gpu = SamplePdhGroup(g_app.metrics.gpu, true);
    g_app.metrics.current.disk = SamplePdhGroup(g_app.metrics.disk, false);
    SampleKeys(g_app.metrics.current);
}

// Overlay visibility, z-order, and tray layout events.
void UpdateLayeredStyle(HWND hwnd) {
    LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (g_app.tray.click_through) {
        ex_style |= WS_EX_TRANSPARENT;
    } else {
        ex_style &= ~WS_EX_TRANSPARENT;
    }
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous_style = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style);
    const DWORD error = previous_style == 0 ? GetLastError() : ERROR_SUCCESS;
    if (previous_style == 0 && error != ERROR_SUCCESS) {
        LogErrorRateLimited(
            L"overlay.update_style",
            kFailureLogIntervalMs,
            L"event=overlay_style_failed stage=click_through error=%lu requested=0x%llx",
            error,
            static_cast<unsigned long long>(ex_style));
    } else {
        LogFailureRecovered(
            L"overlay.update_style",
            L"event=component_recovered component=overlay_style stage=click_through");
    }
}

void KeepOverlayOnTop() {
    if (!g_app.window.hwnd || !IsWindow(g_app.window.hwnd)) {
        return;
    }
    if (g_app.suppression.overlay_suppressed) {
        return;
    }
    if (g_app.placement.maintaining_z_order) {
        return;
    }

    g_app.placement.maintaining_z_order = true;

    if (!IsWindowVisible(g_app.window.hwnd)) {
        ShowWindow(g_app.window.hwnd, SW_SHOWNOACTIVATE);
    }

    const BOOL positioned = SetWindowPos(
        g_app.window.hwnd,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    const DWORD position_error = positioned ? ERROR_SUCCESS : GetLastError();
    g_app.placement.maintaining_z_order = false;
    if (!positioned) {
        LogErrorRateLimited(
            L"z_order.keep_topmost",
            kFailureLogIntervalMs,
            L"event=z_order_failed stage=set_window_pos error=%lu",
            position_error);
    } else {
        LogFailureRecovered(
            L"z_order.keep_topmost",
            L"event=component_recovered component=z_order stage=set_window_pos");
    }
}

const wchar_t* TimerFailureKey(UINT_PTR timer_id) {
    switch (timer_id) {
    case kRefreshTimer:
        return L"timer_install_refresh";
    case kPlacementTimer:
        return L"timer_install_placement";
    case kStateTimer:
        return L"timer_install_state";
    case kZOrderBurstTimer:
        return L"timer_install_z_order_burst";
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
    DWORD error = ERROR_INVALID_WINDOW_HANDLE;
    bool error_available = !hwnd;
    if (hwnd) {
        SetLastError(ERROR_SUCCESS);
        if (SetTimer(hwnd, timer_id, interval_ms, nullptr) != 0) {
            LogFailureRecovered(
                failure_key,
                L"event=timer_install_recovered timer=%ls id=%llu interval_ms=%u",
                timer_name,
                static_cast<unsigned long long>(timer_id),
                interval_ms);
            return true;
        }
        error = GetLastError();
        error_available = error != ERROR_SUCCESS;
    }

    LogErrorRateLimited(
        failure_key,
        kFailureLogIntervalMs,
        L"event=timer_install_failed timer=%ls id=%llu interval_ms=%u "
        L"error=%lu error_available=%d",
        timer_name,
        static_cast<unsigned long long>(timer_id),
        interval_ms,
        error,
        error_available ? 1 : 0);
    return false;
}

void StartZOrderBurst(HWND hwnd) {
    g_app.placement.z_order_burst_until = GetTickCount() + kZOrderBurstDurationMs;
    InstallTimer(hwnd, kZOrderBurstTimer, kZOrderBurstIntervalMs, L"z_order_burst");
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
        if (g_app.window.hwnd && IsWindow(g_app.window.hwnd) && hwnd != g_app.window.hwnd) {
            if (!PostMessageW(g_app.window.hwnd, WM_FOREGROUND_CHANGED, event, reinterpret_cast<LPARAM>(hwnd))) {
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

    if (!g_app.window.hwnd || !IsWindow(g_app.window.hwnd) || !IsTaskbarRelatedWindow(hwnd)) {
        return;
    }

    if (id_object != OBJID_WINDOW && id_object != OBJID_CLIENT) {
        return;
    }

    if (InterlockedCompareExchange(&g_app.placement.tray_layout_update_pending, 1, 0) == 0) {
        if (!PostMessageW(g_app.window.hwnd, WM_TRAY_LAYOUT_CHANGED, 0, 0)) {
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

void AttachToTaskbarOwner(HWND hwnd) {
    if (!kAttachOverlayToTaskbarOwner) {
        if (g_app.placement.taskbar_owner) {
            SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, 0);
            g_app.placement.taskbar_owner = nullptr;
        }
        return;
    }

    HWND taskbar = TaskbarWindow();
    if (!taskbar || taskbar == g_app.placement.taskbar_owner) {
        return;
    }

    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(taskbar));
    g_app.placement.taskbar_owner = taskbar;
}

bool SetPlacementTimer(HWND hwnd, UINT interval_ms, bool fast) {
    if (InstallTimer(hwnd, kPlacementTimer, interval_ms, L"placement")) {
        g_app.placement.timer_fast = fast;
        return true;
    }
    return false;
}

void RestorePlacementTimer(HWND hwnd) {
    if (g_app.placement.timer_fast) {
        SetPlacementTimer(hwnd, kPlacementIntervalMs, false);
    }
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

bool TryGetTrayNotifyRect(RECT& rect) {
    HWND tray = TaskbarWindow();
    HWND notify = tray ? FindDescendantWindow(tray, L"TrayNotifyWnd") : nullptr;
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

    HWND tray = TaskbarWindow();
    HWND notify = tray ? FindDescendantWindow(tray, L"TrayNotifyWnd") : nullptr;
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

void RepositionWindow() {
    RECT taskbar{};
    if (!GetTaskbarRect(taskbar)) {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        taskbar = {work.left, work.bottom - Scale(48, g_app.window.dpi), work.right, work.bottom};
    }

    HMONITOR monitor = MonitorFromRect(&taskbar, MONITOR_DEFAULTTONEAREST);
    g_app.window.dpi = WindowDpi(g_app.window.hwnd);

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

    const BOOL positioned = SetWindowPos(
        g_app.window.hwnd,
        HWND_TOPMOST,
        x,
        y,
        width,
        height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    const DWORD position_error = positioned ? ERROR_SUCCESS : GetLastError();
    if (positioned) {
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
    KeepOverlayOnTop();

    const RECT requested_rect{x, y, x + width, y + height};
    RECT overlay_rect{};
    const bool actual_known = GetWindowRect(g_app.window.hwnd, &overlay_rect) != FALSE;
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
            L"event=placement result=%ls actual_known=1 taskbar=(%ld,%ld,%ld,%ld) anchor_mode=%d "
            L"anchor=(%ld,%ld,%ld,%ld) requested=(%ld,%ld,%ld,%ld) actual=(%ld,%ld,%ld,%ld)",
            positioned ? L"ok" : L"failed",
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

    if (monitor) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(monitor, &mi);
    }
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
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        LogWarningRateLimited(
            L"tray.add",
            kFailureLogIntervalMs,
            L"event=tray_icon_failed operation=add taskbar=%p",
            TaskbarWindow());
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

void ReloadConfigAndRefresh(HWND hwnd) {
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
    if (UpdateOverlaySuppression(hwnd)) {
        return;
    }
    RepositionWindow();
    RenderOverlay(hwnd);
}

void RecoverTaskbar(HWND hwnd) {
    LogInfo(L"event=manual_taskbar_recovery");
    RemoveTrayIcon(hwnd);
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, 0);
    g_app.placement.taskbar_owner = nullptr;

    if (HWND taskbar = TaskbarWindow()) {
        SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(taskbar));
        g_app.placement.taskbar_owner = taskbar;
    }

    AddTrayIcon(hwnd);
    StartZOrderBurst(hwnd);
    if (UpdateOverlaySuppression(hwnd)) {
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    RepositionWindow();
    KeepOverlayOnTop();
    ResetLayeredSurface(hwnd);
    RenderOverlay(hwnd);
}

bool HandleMenuCommand(HWND hwnd, UINT command) {
    switch (command) {
    case ID_OPEN_CONFIG: {
        const std::wstring config_path = ConfigPath();
        const std::wstring module_dir = ModuleDir();
        const INT_PTR shell_result = reinterpret_cast<INT_PTR>(ShellExecuteW(
            hwnd,
            L"open",
            config_path.c_str(),
            nullptr,
            module_dir.c_str(),
            SW_SHOWNORMAL));
        if (shell_result > 32) {
            LogInfo(L"event=user_action action=open_config result=ok");
        } else {
            LogError(
                L"event=user_action action=open_config result=failed shell_result=%lld",
                static_cast<long long>(shell_result));
        }
        return true;
    }
    case ID_RELOAD_CONFIG:
        ReloadConfigAndRefresh(hwnd);
        return true;
    case ID_RECOVER_TASKBAR:
        RecoverTaskbar(hwnd);
        return true;
    case ID_CLICK_THROUGH:
        g_app.tray.click_through = !g_app.tray.click_through;
        UpdateLayeredStyle(hwnd);
        if (((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0) ==
            g_app.tray.click_through) {
            LogInfo(
                L"event=user_action action=click_through result=ok enabled=%d",
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
        if (!DestroyWindow(hwnd)) {
            g_app.window.shutdown_trigger = L"window_destroy";
            LogError(
                L"event=user_action action=exit result=failed error=%lu",
                GetLastError());
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
void LogRenderHresultStage(
    const wchar_t* failure_key,
    const wchar_t* stage,
    HRESULT result) {
    if (FAILED(result)) {
        LogErrorRateLimited(
            failure_key,
            kFailureLogIntervalMs,
            L"event=render_failed stage=%ls hresult=0x%08lx",
            stage,
            static_cast<unsigned long>(result));
        return;
    }

    LogFailureRecovered(
        failure_key,
        L"event=component_recovered component=render stage=%ls",
        stage);
}

HRESULT EnsureRenderResources() {
    RenderResources& render = g_app.render.resources;
    HRESULT hr = S_OK;
    const bool dpi_changed = render.text_format_dpi != g_app.window.dpi;

    if (!render.d2d_factory) {
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &render.d2d_factory);
        LogRenderHresultStage(L"render.d2d_factory", L"create_d2d_factory", hr);
        if (FAILED(hr)) {
            return hr;
        }
    }

    if (!render.dwrite_factory) {
        IUnknown* factory = nullptr;
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &factory);
        LogRenderHresultStage(L"render.dwrite_factory", L"create_dwrite_factory", hr);
        if (FAILED(hr)) {
            return hr;
        }
        render.dwrite_factory = static_cast<IDWriteFactory*>(factory);
    }

    if (!render.wic_factory) {
        hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            __uuidof(IWICImagingFactory),
            reinterpret_cast<void**>(&render.wic_factory));
        LogRenderHresultStage(L"render.wic_factory", L"create_wic_factory", hr);
        if (FAILED(hr)) {
            return hr;
        }
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
        LogRenderHresultStage(L"render.text_format", L"create_text_format", hr);
        if (FAILED(hr)) {
            return hr;
        }

        const HRESULT alignment_result =
            render.text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        LogRenderHresultStage(
            L"render.text_format_alignment",
            L"configure_text_format_alignment",
            alignment_result);
        if (FAILED(alignment_result)) {
            SafeRelease(render.text_format);
            return alignment_result;
        }
        const HRESULT wrapping_result =
            render.text_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        LogRenderHresultStage(
            L"render.text_format_wrapping",
            L"configure_text_format_wrapping",
            wrapping_result);
        if (FAILED(wrapping_result)) {
            SafeRelease(render.text_format);
            return wrapping_result;
        }
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
        LogRenderHresultStage(L"render.arrow_text_format", L"create_arrow_text_format", hr);
        if (FAILED(hr)) {
            return hr;
        }

        const HRESULT alignment_result =
            render.arrow_text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        LogRenderHresultStage(
            L"render.arrow_text_format_alignment",
            L"configure_arrow_text_format_alignment",
            alignment_result);
        if (FAILED(alignment_result)) {
            SafeRelease(render.arrow_text_format);
            return alignment_result;
        }
        const HRESULT wrapping_result =
            render.arrow_text_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        LogRenderHresultStage(
            L"render.arrow_text_format_wrapping",
            L"configure_arrow_text_format_wrapping",
            wrapping_result);
        if (FAILED(wrapping_result)) {
            SafeRelease(render.arrow_text_format);
            return wrapping_result;
        }
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
        LogRenderHresultStage(L"render.key_text_format", L"create_key_text_format", hr);
        if (FAILED(hr)) {
            return hr;
        }

        const HRESULT alignment_result =
            render.key_text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        LogRenderHresultStage(
            L"render.key_text_format_alignment",
            L"configure_key_text_format_alignment",
            alignment_result);
        if (FAILED(alignment_result)) {
            SafeRelease(render.key_text_format);
            return alignment_result;
        }
        const HRESULT wrapping_result =
            render.key_text_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        LogRenderHresultStage(
            L"render.key_text_format_wrapping",
            L"configure_key_text_format_wrapping",
            wrapping_result);
        if (FAILED(wrapping_result)) {
            SafeRelease(render.key_text_format);
            return wrapping_result;
        }
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
bool PresentPixels(HWND hwnd, const BYTE* source_pixels, int width, int height) {
    RECT window_rect{};
    SetLastError(ERROR_SUCCESS);
    if (!GetWindowRect(hwnd, &window_rect)) {
        const DWORD error = GetLastError();
        LogErrorRateLimited(
            L"present.get_window_rect",
            kFailureLogIntervalMs,
            L"event=present_failed stage=get_window_rect error=%lu error_available=%d width=%d height=%d",
            error,
            error != ERROR_SUCCESS ? 1 : 0,
            width,
            height);
        return false;
    }
    if (width <= 0 || height <= 0 || !source_pixels) {
        LogErrorRateLimited(
            L"present.invalid_input",
            kFailureLogIntervalMs,
            L"event=present_failed stage=validate error=%lu width=%d height=%d",
            static_cast<unsigned long>(ERROR_INVALID_PARAMETER),
            width,
            height);
        return false;
    }

    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) {
        LogErrorRateLimited(
            L"present.get_screen_dc",
            kFailureLogIntervalMs,
            L"event=present_failed stage=get_screen_dc");
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
    HDC mem_dc = CreateCompatibleDC(screen_dc);
    if (!mem_dc) {
        ReleaseDC(nullptr, screen_dc);
        LogErrorRateLimited(
            L"present.create_memory_dc",
            kFailureLogIntervalMs,
            L"event=present_failed stage=create_memory_dc");
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
        LogErrorRateLimited(
            L"present.create_bitmap",
            kFailureLogIntervalMs,
            L"event=present_failed stage=create_bitmap error=%lu error_available=%d",
            error,
            error != ERROR_SUCCESS ? 1 : 0);
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
        LogErrorRateLimited(
            L"present.update_layered_window",
            kFailureLogIntervalMs,
            L"event=present_failed stage=update_layered_window error=%lu error_available=%d width=%d height=%d",
            update_error,
            update_error != ERROR_SUCCESS ? 1 : 0,
            width,
            height);
    } else {
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
                L"event=component_recovered component=present");
        }
    }
    return updated != FALSE;
}

bool ReapplyLastFrame(HWND hwnd) {
    if (g_app.render.last_frame.empty()) {
        return false;
    }
    return PresentPixels(hwnd, g_app.render.last_frame.data(), g_app.render.last_frame_width, g_app.render.last_frame_height);
}

void ResetLayeredSurface(HWND hwnd) {
    const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR clear_result =
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style & ~WS_EX_LAYERED);
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
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    const DWORD position_error = positioned ? ERROR_SUCCESS : GetLastError();
    if (!positioned) {
        LogErrorRateLimited(
            L"overlay.reset_layered_position",
            kFailureLogIntervalMs,
            L"event=overlay_surface_reset_failed stage=set_window_pos error=%lu",
            position_error);
    } else {
        LogFailureRecovered(
            L"overlay.reset_layered_position",
            L"event=component_recovered component=overlay_surface stage=set_window_pos");
    }
    ReapplyLastFrame(hwnd);
}

bool UpdateOverlaySuppression(HWND hwnd) {
    const wchar_t* reason = OverlaySuppressionReason();
    const bool should_suppress = reason != nullptr;
    if (should_suppress) {
        if (!g_app.suppression.overlay_suppressed) {
            LogInfo(L"event=suppression_on reason=%ls", reason);
            if (std::wcscmp(reason, L"fullscreen_window") == 0) {
                LogFullscreenSuppressionContext(L"enter");
            }
        }
        g_app.suppression.overlay_suppressed = true;
        RestorePlacementTimer(hwnd);
        if (IsWindowVisible(hwnd)) {
            ShowWindow(hwnd, SW_HIDE);
        }
        return true;
    }

    if (g_app.suppression.overlay_suppressed) {
        g_app.suppression.overlay_suppressed = false;
        LogInfo(L"event=suppression_off");
        LogFullscreenSuppressionContext(L"exit");
        RestorePlacementTimer(hwnd);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        RepositionWindow();
        ReapplyLastFrame(hwnd);
        RenderOverlay(hwnd);
        return true;
    }

    return false;
}

bool UpdateFreezeState(HWND hwnd) {
    if (ShouldFreezeOverlayUpdate()) {
        if (!g_app.suppression.overlay_update_frozen) {
            LogInfo(L"event=freeze_on reason=screenshot");
        }
        g_app.suppression.overlay_update_frozen = true;
        ReapplyLastFrame(hwnd);
        return true;
    }

    if (g_app.suppression.overlay_update_frozen) {
        g_app.suppression.overlay_update_frozen = false;
        LogInfo(L"event=freeze_off");
        g_app.suppression.refresh_resume_tick = GetTickCount() + 800;
        ReapplyLastFrame(hwnd);
        KeepOverlayOnTop();
        return true;
    }

    return false;
}

bool HandleOverlayStateGuards(HWND hwnd) {
    if (UpdateOverlaySuppression(hwnd)) {
        return true;
    }
    if (UpdateFreezeState(hwnd)) {
        return true;
    }
    if (g_app.suppression.refresh_resume_tick != 0 && !TickPassed(GetTickCount(), g_app.suppression.refresh_resume_tick)) {
        ReapplyLastFrame(hwnd);
        KeepOverlayOnTop();
        return true;
    }

    g_app.suppression.refresh_resume_tick = 0;
    return false;
}

void RenderOverlay(HWND hwnd) {
    RECT client{};
    SetLastError(ERROR_SUCCESS);
    if (!GetClientRect(hwnd, &client)) {
        const DWORD error = GetLastError();
        LogErrorRateLimited(
            L"render.get_client_rect",
            kFailureLogIntervalMs,
            L"event=render_failed stage=get_client_rect error=%lu error_available=%d",
            error,
            error != ERROR_SUCCESS ? 1 : 0);
        return;
    }
    LogFailureRecovered(
        L"render.get_client_rect",
        L"event=render_recovered stage=get_client_rect");

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        LogWarningRateLimited(
            L"render.invalid_client_size",
            kFailureLogIntervalMs,
            L"event=render_skipped stage=validate_client_size width=%d height=%d",
            width,
            height);
        return;
    }
    LogFailureRecovered(
        L"render.invalid_client_size",
        L"event=render_recovered stage=validate_client_size width=%d height=%d",
        width,
        height);

    if (FAILED(EnsureRenderResources())) {
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
    LogRenderHresultStage(L"render.create_bitmap", L"create_wic_bitmap", hr);
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
        LogRenderHresultStage(
            L"render.create_target",
            L"create_wic_bitmap_render_target",
            hr);
    }
    if (SUCCEEDED(hr)) {
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        D2D1_COLOR_F color{1.0f, 1.0f, 1.0f, 1.0f};
        hr = target->CreateSolidColorBrush(color, &brush);
        LogRenderHresultStage(L"render.create_brush", L"create_solid_color_brush", hr);
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
        LogRenderHresultStage(L"render.end_draw", L"end_draw", hr);
    }

    if (SUCCEEDED(hr)) {
        WICRect rect{0, 0, width, height};
        std::vector<BYTE> frame(static_cast<size_t>(width) * height * 4);
        hr = wic_bitmap->CopyPixels(&rect, width * 4, static_cast<UINT>(frame.size()), frame.data());
        LogRenderHresultStage(L"render.copy_pixels", L"copy_pixels", hr);
        if (SUCCEEDED(hr)) {
            bool has_visible_pixels = false;
            for (size_t i = 3; i < frame.size(); i += 4) {
                if (frame[i] != 0) {
                    has_visible_pixels = true;
                    break;
                }
            }
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
        }
        if (SUCCEEDED(hr) && PresentPixels(hwnd, frame.data(), width, height)) {
            g_app.render.last_frame = std::move(frame);
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

LRESULT HandleTaskbarCreated(HWND hwnd) {
    LogInfo(L"event=taskbar_created");
    if (!g_app.window.monitor_initialized) {
        return 0;
    }

    AttachToTaskbarOwner(hwnd);
    AddTrayIcon(hwnd);
    if (UpdateOverlaySuppression(hwnd)) {
        return 0;
    }
    RepositionWindow();
    RenderOverlay(hwnd);
    return 0;
}

void InitializeMonitor(HWND hwnd) {
    if (g_app.window.monitor_initialized) {
        return;
    }

    g_app.window.monitor_initialized = true;
    LogInfo(L"event=monitor_init delayed=%d", g_app.window.launched_at_startup ? 1 : 0);
    AddTrayIcon(hwnd);
    AttachToTaskbarOwner(hwnd);
    RegisterTrayEventHooks();
    InitPdhGroup(g_app.metrics.gpu, L"\\GPU Engine(*)\\Utilization Percentage");
    InitPdhGroup(g_app.metrics.disk, L"\\PhysicalDisk(_Total)\\% Disk Time");
    SampleMetrics();
    const bool refresh_timer = InstallTimer(hwnd, kRefreshTimer, 1000, L"refresh");
    const bool placement_timer = SetPlacementTimer(hwnd, kPlacementIntervalMs, false);
    const bool state_timer = InstallTimer(hwnd, kStateTimer, kStateIntervalMs, L"state");
    LogInfo(
        L"event=timers_initialized refresh=%d placement=%d state=%d",
        refresh_timer ? 1 : 0,
        placement_timer ? 1 : 0,
        state_timer ? 1 : 0);
    if (UpdateOverlaySuppression(hwnd)) {
        return;
    }
    RepositionWindow();
    RenderOverlay(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
}

LRESULT HandleCreate(HWND hwnd) {
    g_app.window.hwnd = hwnd;
    g_app.window.dpi = WindowDpi(hwnd);
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

    if (timer_id == kZOrderBurstTimer) {
        if (g_app.suppression.overlay_suppressed ||
            g_app.placement.z_order_burst_until == 0 ||
            TickPassed(GetTickCount(), g_app.placement.z_order_burst_until)) {
            KillTimer(hwnd, kZOrderBurstTimer);
            g_app.placement.z_order_burst_until = 0;
            return 0;
        }

        RepositionWindow();
        KeepOverlayOnTop();
        ReapplyLastFrame(hwnd);
        return 0;
    }

    if (timer_id == kStateTimer) {
        if (HandleOverlayStateGuards(hwnd)) {
            return 0;
        }
        if (g_app.config.show_key_widget && SampleKeysIfChanged()) {
            RenderOverlay(hwnd);
        }
        return 0;
    }

    if (HandleOverlayStateGuards(hwnd)) {
        return 0;
    }

    if (timer_id == kRefreshTimer) {
        SampleMetrics();
        RepositionWindow();
        KeepOverlayOnTop();
        RenderOverlay(hwnd);
    } else if (timer_id == kPlacementTimer) {
        RepositionWindow();
        KeepOverlayOnTop();
    }
    return 0;
}

LRESULT HandleDpiChanged(HWND hwnd, WPARAM wparam, LPARAM lparam) {
    g_app.window.dpi = HIWORD(wparam);
    if (lparam) {
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(
            hwnd,
            HWND_TOPMOST,
            suggested->left,
            suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOACTIVATE);
    }
    if (UpdateOverlaySuppression(hwnd)) {
        return 0;
    }
    RepositionWindow();
    RenderOverlay(hwnd);
    return 0;
}

LRESULT HandleDisplayChange(HWND hwnd) {
    if (!g_app.window.monitor_initialized) {
        return 0;
    }

    AttachToTaskbarOwner(hwnd);
    if (UpdateOverlaySuppression(hwnd)) {
        return 0;
    }
    RepositionWindow();
    RenderOverlay(hwnd);
    return 0;
}

LRESULT HandleSettingChange(HWND hwnd, LPARAM lparam) {
    if (lparam && std::wcscmp(reinterpret_cast<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0) {
        EnableSystemMenuTheme();
    }
    return HandleDisplayChange(hwnd);
}

LRESULT HandleShowWindow(HWND hwnd, WPARAM wparam) {
    if (!g_app.window.monitor_initialized) {
        return 0;
    }

    LogInfo(
        L"event=show_window shown=%d suppressed=%d",
        wparam != 0 ? 1 : 0,
        g_app.suppression.overlay_suppressed ? 1 : 0);

    if (!wparam) {
        if (!g_app.suppression.overlay_suppressed) {
            SetPlacementTimer(hwnd, 100, true);
            StartZOrderBurst(hwnd);
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            ReapplyLastFrame(hwnd);
            KeepOverlayOnTop();
        }
    } else {
        SetPlacementTimer(hwnd, kPlacementIntervalMs, false);
        if (HandleOverlayStateGuards(hwnd)) {
            return 0;
        }
        RenderOverlay(hwnd);
    }
    return 0;
}

LRESULT HandleTrayLayoutChanged(HWND hwnd) {
    InterlockedExchange(&g_app.placement.tray_layout_update_pending, 0);
    if (!g_app.window.monitor_initialized) {
        return 0;
    }

    LogInfo(L"event=tray_layout_changed");
    StartZOrderBurst(hwnd);
    AttachToTaskbarOwner(hwnd);
    if (UpdateOverlaySuppression(hwnd)) {
        return 0;
    }
    RepositionWindow();
    KeepOverlayOnTop();
    RenderOverlay(hwnd);
    return 0;
}

LRESULT HandleForegroundChanged(HWND hwnd, WPARAM source_event, LPARAM source_lparam) {
    if (!g_app.window.monitor_initialized) {
        return 0;
    }

    const HWND source = reinterpret_cast<HWND>(source_lparam);
    const std::wstring source_exe = WindowProcessBasename(source);
    const std::wstring source_class = WindowClassName(source);
    LogInfo(
        L"event=foreground_changed source_event=%llu source_exe=%ls source_class=%ls overlay_visible=%d",
        static_cast<unsigned long long>(source_event),
        source_exe.c_str(),
        source_class.c_str(),
        IsWindowVisible(hwnd) ? 1 : 0);

    const bool control_center_foreground =
        source_exe == L"shellhost.exe" && source_class == L"ControlCenterWindow";
    const bool control_center_closed =
        g_app.placement.control_center_was_foreground &&
        source_exe == L"explorer.exe" && source_class == L"Shell_TrayWnd";
    g_app.placement.control_center_was_foreground = control_center_foreground;

    StartZOrderBurst(hwnd);
    RepositionWindow();
    KeepOverlayOnTop();
    ReapplyLastFrame(hwnd);
    if (control_center_closed) {
        LogInfo(L"event=control_center_closed reset_layered_surface=1");
        ResetLayeredSurface(hwnd);
    }

    const HWND above = GetWindow(hwnd, GW_HWNDPREV);
    RECT above_rect{};
    GetWindowRect(above, &above_rect);
    const std::wstring above_exe = WindowProcessBasename(above);
    const std::wstring above_class = WindowClassName(above);
    LogInfo(
        L"event=z_order above_exe=%ls above_class=%ls above_visible=%d above_rect=(%ld,%ld,%ld,%ld)",
        above_exe.c_str(),
        above_class.c_str(),
        above && IsWindowVisible(above) ? 1 : 0,
        above_rect.left,
        above_rect.top,
        above_rect.right,
        above_rect.bottom);
    return 0;
}

LRESULT HandleTrayIcon(HWND hwnd, LPARAM lparam) {
    if (LOWORD(lparam) == WM_CONTEXTMENU) {
        ShowTrayMenu(hwnd);
    } else if (LOWORD(lparam) == WM_LBUTTONDBLCLK) {
        RepositionWindow();
    }
    return 0;
}

LRESULT HandleDestroy(HWND hwnd) {
    KillTimer(hwnd, kRefreshTimer);
    KillTimer(hwnd, kPlacementTimer);
    KillTimer(hwnd, kStateTimer);
    KillTimer(hwnd, kZOrderBurstTimer);
    KillTimer(hwnd, kStartupInitTimer);
    UnregisterTrayEventHooks();
    RemoveTrayIcon(hwnd);
    if (g_app.metrics.gpu.query) {
        PdhCloseQuery(g_app.metrics.gpu.query);
    }
    if (g_app.metrics.disk.query) {
        PdhCloseQuery(g_app.metrics.disk.query);
    }
    ReleaseRenderResources();
    simple_monitor::FlushLogSummaries();
    LogInfo(
        L"event=shutdown build=stable trigger=%ls",
        g_app.window.shutdown_trigger ? g_app.window.shutdown_trigger : L"unknown");
    PostQuitMessage(0);
    return 0;
}

// Window message handling and process startup.
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == g_app.window.taskbar_created) {
        return HandleTaskbarCreated(hwnd);
    }

    switch (msg) {
    case WM_CREATE:
        return HandleCreate(hwnd);

    case WM_TIMER:
        return HandleTimer(hwnd, static_cast<UINT_PTR>(wparam));

    case WM_PAINT:
        ValidatePaint(hwnd);
        return 0;

    case WM_DPICHANGED:
        return HandleDpiChanged(hwnd, wparam, lparam);

    case WM_DISPLAYCHANGE:
    case WM_DEVICECHANGE:
        return HandleDisplayChange(hwnd);

    case WM_SETTINGCHANGE:
        return HandleSettingChange(hwnd, lparam);

    case WM_WINDOWPOSCHANGED:
        KeepOverlayOnTop();
        break;

    case WM_SHOWWINDOW:
        return HandleShowWindow(hwnd, wparam);

    case WM_COMMAND:
        HandleMenuCommand(hwnd, LOWORD(wparam));
        return 0;

    case WM_TRAY_LAYOUT_CHANGED:
        return HandleTrayLayoutChanged(hwnd);

    case WM_FOREGROUND_CHANGED:
        return HandleForegroundChanged(hwnd, wparam, lparam);

    case WM_TRAYICON:
        return HandleTrayIcon(hwnd, lparam);

    case WM_DESTROY:
        return HandleDestroy(hwnd);
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hIcon = LoadAppIcon(instance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hIconSm = LoadAppIcon(instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    if (RegisterClassExW(&wc) == 0) {
        LogError(
            L"event=startup_component_failed component=window_class stage=overlay error=%lu",
            GetLastError());
        return false;
    }
    return true;
}

int Run(HINSTANCE instance) {
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
    g_app.suppression.startup_warmup_until = GetTickCount() + kStartupWarmupMs;
    LogInfo(
        L"event=startup schema=2 build=stable launch=%ls warmup_ms=%lu taskbar_message_id=%u "
        L"com_hresult=0x%08lx",
        g_app.window.launched_at_startup ? L"startup" : L"manual",
        kStartupWarmupMs,
        g_app.window.taskbar_created,
        static_cast<unsigned long>(co_result));
    LogConfigSnapshot(L"config_loaded");
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

    if (!RegisterWindowClass(instance)) {
        LogError(L"event=startup_failed stage=register_window_class");
        if (g_app.window.com_initialized) {
            CoUninitialize();
        }
        return 1;
    }

    const DWORD ex_style =
        WS_EX_TOOLWINDOW |
        WS_EX_TOPMOST |
        WS_EX_LAYERED |
        WS_EX_NOACTIVATE;

    HWND hwnd = CreateWindowExW(
        ex_style,
        kWindowClass,
        L"Simple Monitor",
        WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        430,
        44,
        TaskbarWindow(),
        nullptr,
        instance,
        nullptr);

    if (!hwnd) {
        LogError(
            L"event=startup_failed stage=create_overlay error=%lu",
            GetLastError());
        if (g_app.window.com_initialized) {
            CoUninitialize();
        }
        return 1;
    }

    if (!g_app.window.launched_at_startup) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
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
