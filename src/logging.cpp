#include "logging.h"

#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace simple_monitor {
namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr ULONGLONG kMaxLogBytes = 2ULL * 1024ULL * 1024ULL;
constexpr auto kInternalDiagnosticInterval = std::chrono::seconds(60);
constexpr auto kRotationRetryInterval = std::chrono::seconds(30);
constexpr auto kRecoveryRetryInterval = std::chrono::seconds(30);
constexpr auto kOverflowLogInterval = std::chrono::seconds(60);
constexpr size_t kMaxRateLimitEntries = 64;
constexpr DWORD kLogMutexWaitMs = 100;

struct RateLimitEntry {
    LogLevel level = LogLevel::Warning;
    SteadyClock::time_point next_allowed{};
    SteadyClock::time_point first_seen{};
    SteadyClock::time_point next_recovery_retry{};
    std::uint64_t total = 0;
    std::uint64_t pending_suppressed = 0;
    std::uint64_t total_suppressed = 0;
    bool active = false;
    bool failure_logged = false;
};

struct LoggingState {
    CRITICAL_SECTION lock{};
    bool enabled = false;
    LogLevel minimum_level = LogLevel::Info;
    std::wstring path;
    std::wstring session_id;
    SteadyClock::time_point session_started{};
    std::uint64_t sequence = 0;
    std::unordered_map<std::wstring, RateLimitEntry> rate_limits;
    RateLimitEntry overflow_rate_limit;
    HANDLE file_mutex = nullptr;
    std::wstring mutex_path;
    std::uint64_t pending_internal_failures = 0;
    DWORD last_internal_error = ERROR_SUCCESS;
    std::wstring last_internal_stage;
    SteadyClock::time_point next_internal_diagnostic{};
    SteadyClock::time_point next_rotation_retry{};

    LoggingState() {
        InitializeCriticalSection(&lock);
    }

    ~LoggingState() {
        if (file_mutex) {
            CloseHandle(file_mutex);
        }
        DeleteCriticalSection(&lock);
    }

    LoggingState(const LoggingState&) = delete;
    LoggingState& operator=(const LoggingState&) = delete;
};

LoggingState g_logging;

class ExclusiveLogLock {
public:
    explicit ExclusiveLogLock(CRITICAL_SECTION& lock) : lock_(lock) {
        EnterCriticalSection(&lock_);
    }

    ~ExclusiveLogLock() {
        LeaveCriticalSection(&lock_);
    }

    ExclusiveLogLock(const ExclusiveLogLock&) = delete;
    ExclusiveLogLock& operator=(const ExclusiveLogLock&) = delete;

private:
    CRITICAL_SECTION& lock_;
};

class InterprocessLogLock {
public:
    explicit InterprocessLogLock(HANDLE mutex) : mutex_(mutex) {
        if (!mutex_) {
            acquired_ = true;
            return;
        }

        const DWORD result = WaitForSingleObject(mutex_, kLogMutexWaitMs);
        acquired_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        if (!acquired_) {
            error_ = result == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
        }
    }

    ~InterprocessLogLock() {
        if (mutex_ && acquired_) {
            ReleaseMutex(mutex_);
        }
    }

    bool acquired() const {
        return acquired_;
    }

    DWORD error() const {
        return error_;
    }

    InterprocessLogLock(const InterprocessLogLock&) = delete;
    InterprocessLogLock& operator=(const InterprocessLogLock&) = delete;

private:
    HANDLE mutex_ = nullptr;
    bool acquired_ = false;
    DWORD error_ = ERROR_SUCCESS;
};

bool LevelEnabled(LogLevel level) {
    return static_cast<int>(level) >= static_cast<int>(g_logging.minimum_level);
}

std::wstring LogMutexName(const std::wstring& path) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (wchar_t ch : path) {
        const wchar_t normalized = static_cast<wchar_t>(std::towlower(ch));
        hash ^= static_cast<std::uint16_t>(normalized);
        hash *= 1099511628211ULL;
    }

    wchar_t name[64]{};
    _snwprintf_s(
        name,
        _countof(name),
        _TRUNCATE,
        L"Local\\SimpleMonitorLog-%016llx",
        static_cast<unsigned long long>(hash));
    return name;
}

const wchar_t* LogLevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return L"DEBUG";
    case LogLevel::Warning:
        return L"WARN";
    case LogLevel::Error:
        return L"ERROR";
    case LogLevel::Info:
    default:
        return L"INFO";
    }
}

void StartSessionLocked() {
    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t session_id[64]{};
    _snwprintf_s(
        session_id,
        _countof(session_id),
        _TRUNCATE,
        L"%04u%02u%02u-%02u%02u%02u.%03u-%lu",
        static_cast<unsigned>(st.wYear),
        static_cast<unsigned>(st.wMonth),
        static_cast<unsigned>(st.wDay),
        static_cast<unsigned>(st.wHour),
        static_cast<unsigned>(st.wMinute),
        static_cast<unsigned>(st.wSecond),
        static_cast<unsigned>(st.wMilliseconds),
        GetCurrentProcessId());

    g_logging.session_id = session_id;
    g_logging.session_started = SteadyClock::now();
    g_logging.sequence = 0;
    g_logging.rate_limits.clear();
    g_logging.overflow_rate_limit = RateLimitEntry{};
}

void EnsureSessionLocked() {
    if (g_logging.session_id.empty()) {
        StartSessionLocked();
    }
}

void ReportInternalFailureLocked(const wchar_t* stage, DWORD error) {
    ++g_logging.pending_internal_failures;
    g_logging.last_internal_error = error;
    g_logging.last_internal_stage = stage ? stage : L"unknown";

    const auto now = SteadyClock::now();
    if (g_logging.next_internal_diagnostic != SteadyClock::time_point{} &&
        now < g_logging.next_internal_diagnostic) {
        return;
    }
    g_logging.next_internal_diagnostic = now + kInternalDiagnosticInterval;

    wchar_t message[512]{};
    _snwprintf_s(
        message,
        _countof(message),
        _TRUNCATE,
        L"[SimpleMonitorLogger] stage=%ls error=%lu failures=%llu\r\n",
        g_logging.last_internal_stage.c_str(),
        error,
        static_cast<unsigned long long>(g_logging.pending_internal_failures));
    OutputDebugStringW(message);
}

bool RotateLogLocked(bool force) {
    if (g_logging.path.empty()) {
        return true;
    }

    const auto now = SteadyClock::now();
    if (!force &&
        g_logging.next_rotation_retry != SteadyClock::time_point{} &&
        now < g_logging.next_rotation_retry) {
        return true;
    }

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(g_logging.path.c_str(), GetFileExInfoStandard, &attributes)) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            g_logging.next_rotation_retry = SteadyClock::time_point{};
            return true;
        }
        ReportInternalFailureLocked(L"stat", error);
        g_logging.next_rotation_retry = now + kRotationRetryInterval;
        return false;
    }

    ULARGE_INTEGER size{};
    size.HighPart = attributes.nFileSizeHigh;
    size.LowPart = attributes.nFileSizeLow;
    if (size.QuadPart == 0 || (!force && size.QuadPart < kMaxLogBytes)) {
        return true;
    }

    const std::wstring backup_path = g_logging.path + L".1";
    if (!MoveFileExW(
            g_logging.path.c_str(),
            backup_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ReportInternalFailureLocked(L"rotate", GetLastError());
        g_logging.next_rotation_retry = now + kRotationRetryInterval;
        return false;
    }
    g_logging.next_rotation_retry = SteadyClock::time_point{};
    return true;
}

void AppendSuffix(
    wchar_t* suffix,
    size_t suffix_count,
    const wchar_t* format,
    ...) {
    const size_t used = std::wcslen(suffix);
    if (used >= suffix_count - 1) {
        return;
    }

    va_list args;
    va_start(args, format);
    _vsnwprintf_s(suffix + used, suffix_count - used, _TRUNCATE, format, args);
    va_end(args);
}

bool WriteMessageLocked(
    LogLevel level,
    const wchar_t* message,
    bool message_truncated,
    std::uint64_t suppressed,
    bool bypass_level_filter = false) {
    if (!g_logging.enabled || g_logging.path.empty() ||
        (!bypass_level_filter && !LevelEnabled(level))) {
        return false;
    }

    InterprocessLogLock file_lock(g_logging.file_mutex);
    if (!file_lock.acquired()) {
        ReportInternalFailureLocked(L"mutex_wait", file_lock.error());
        return false;
    }

    EnsureSessionLocked();
    RotateLogLocked(false);

    const std::uint64_t sequence = ++g_logging.sequence;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        SteadyClock::now() - g_logging.session_started).count();

    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t suffix[320]{};
    if (message_truncated) {
        AppendSuffix(suffix, _countof(suffix), L" message_truncated=1");
    }
    if (suppressed > 0) {
        AppendSuffix(
            suffix,
            _countof(suffix),
            L" suppressed=%llu",
            static_cast<unsigned long long>(suppressed));
    }
    if (g_logging.pending_internal_failures > 0) {
        AppendSuffix(
            suffix,
            _countof(suffix),
            L" logger_failures=%llu logger_stage=%ls logger_error=%lu",
            static_cast<unsigned long long>(g_logging.pending_internal_failures),
            g_logging.last_internal_stage.c_str(),
            g_logging.last_internal_error);
    }

    wchar_t line[3584]{};
    _snwprintf_s(
        line,
        _countof(line),
        _TRUNCATE,
        L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%ls] "
        L"[pid=%lu tid=%lu session=%ls seq=%llu elapsed_ms=%lld] %ls%ls\r\n",
        static_cast<unsigned>(st.wYear),
        static_cast<unsigned>(st.wMonth),
        static_cast<unsigned>(st.wDay),
        static_cast<unsigned>(st.wHour),
        static_cast<unsigned>(st.wMinute),
        static_cast<unsigned>(st.wSecond),
        static_cast<unsigned>(st.wMilliseconds),
        LogLevelName(level),
        GetCurrentProcessId(),
        GetCurrentThreadId(),
        g_logging.session_id.c_str(),
        static_cast<unsigned long long>(sequence),
        static_cast<long long>(elapsed),
        message ? message : L"",
        suffix);

    HANDLE file = CreateFileW(
        g_logging.path.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ReportInternalFailureLocked(L"open", GetLastError());
        return false;
    }

    bool write_succeeded = false;
    const int line_chars = static_cast<int>(std::wcslen(line));
    const int byte_count = WideCharToMultiByte(
        CP_UTF8,
        0,
        line,
        line_chars,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (byte_count <= 0) {
        ReportInternalFailureLocked(L"encode", GetLastError());
    } else {
        std::vector<char> buffer(static_cast<size_t>(byte_count));
        if (WideCharToMultiByte(
                CP_UTF8,
                0,
                line,
                line_chars,
                buffer.data(),
                byte_count,
                nullptr,
                nullptr) <= 0) {
            ReportInternalFailureLocked(L"encode", GetLastError());
        } else {
            size_t offset = 0;
            while (offset < buffer.size()) {
                DWORD written = 0;
                const BOOL write_result = WriteFile(
                    file,
                    buffer.data() + offset,
                    static_cast<DWORD>(buffer.size() - offset),
                    &written,
                    nullptr);
                if (!write_result || written == 0) {
                    const DWORD error = write_result ? ERROR_WRITE_FAULT : GetLastError();
                    ReportInternalFailureLocked(L"write", error);
                    break;
                }
                offset += written;
            }
            write_succeeded = offset == buffer.size();
        }
    }
    CloseHandle(file);

    if (write_succeeded) {
        g_logging.pending_internal_failures = 0;
        g_logging.last_internal_error = ERROR_SUCCESS;
        g_logging.last_internal_stage.clear();
    }
    return write_succeeded;
}

void LogMessage(LogLevel level, const wchar_t* format, va_list args) {
    if (!format) {
        return;
    }

    ExclusiveLogLock lock(g_logging.lock);
    if (!g_logging.enabled || g_logging.path.empty() || !LevelEnabled(level)) {
        return;
    }

    wchar_t message[2048]{};
    const int format_result =
        _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    WriteMessageLocked(level, message, format_result < 0, 0);
}

void LogRateLimitedMessage(
    LogLevel level,
    const wchar_t* key,
    unsigned interval_ms,
    const wchar_t* format,
    va_list args) {
    if (!format) {
        return;
    }

    ExclusiveLogLock lock(g_logging.lock);
    if (!g_logging.enabled || g_logging.path.empty() || !LevelEnabled(level)) {
        return;
    }
    if (!key || *key == L'\0' || interval_ms == 0) {
        wchar_t message[2048]{};
        const int format_result =
            _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
        WriteMessageLocked(level, message, format_result < 0, 0);
        return;
    }

    const auto now = SteadyClock::now();
    RateLimitEntry* entry = nullptr;
    bool overflow_entry = false;
    auto found = g_logging.rate_limits.find(key);
    if (found != g_logging.rate_limits.end()) {
        entry = &found->second;
    } else if (g_logging.rate_limits.size() < kMaxRateLimitEntries) {
        entry = &g_logging.rate_limits.emplace(key, RateLimitEntry{}).first->second;
    } else {
        entry = &g_logging.overflow_rate_limit;
        overflow_entry = true;
    }

    if (!entry->active) {
        entry->active = true;
        entry->failure_logged = false;
    }
    if (entry->first_seen == SteadyClock::time_point{}) {
        entry->first_seen = now;
        entry->level = level;
    } else if (static_cast<int>(level) > static_cast<int>(entry->level)) {
        entry->level = level;
    }
    ++entry->total;
    if (entry->next_allowed != SteadyClock::time_point{} && now < entry->next_allowed) {
        ++entry->pending_suppressed;
        ++entry->total_suppressed;
        return;
    }

    wchar_t message[2048]{};
    const int format_result =
        _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    wchar_t overflow_message[2560]{};
    const wchar_t* output_message = message;
    bool output_truncated = format_result < 0;
    if (overflow_entry) {
        const int overflow_result = _snwprintf_s(
            overflow_message,
            _countof(overflow_message),
            _TRUNCATE,
            L"%ls rate_limit_overflow=1 rate_limit_key=%ls capacity=%llu",
            message,
            key,
            static_cast<unsigned long long>(kMaxRateLimitEntries));
        output_message = overflow_message;
        output_truncated = output_truncated || overflow_result < 0;
    }

    const LogLevel output_level = overflow_entry ? entry->level : level;
    if (WriteMessageLocked(
            output_level,
            output_message,
            output_truncated,
            entry->pending_suppressed)) {
        entry->pending_suppressed = 0;
        entry->failure_logged = true;
        if (overflow_entry) {
            entry->level = LogLevel::Warning;
        }
    } else {
        ++entry->pending_suppressed;
        ++entry->total_suppressed;
    }
    entry->next_allowed = now +
        (overflow_entry ? kOverflowLogInterval : std::chrono::milliseconds(interval_ms));
}

}  // namespace

LogLevel ParseLogLevel(const std::wstring& value) {
    std::wstring lowered = value;
    for (wchar_t& ch : lowered) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    if (lowered == L"debug") {
        return LogLevel::Debug;
    }
    if (lowered == L"warning" || lowered == L"warn") {
        return LogLevel::Warning;
    }
    if (lowered == L"error") {
        return LogLevel::Error;
    }
    return LogLevel::Info;
}

void ConfigureLogging(bool enabled, std::wstring path, LogLevel minimum_level) {
    ExclusiveLogLock lock(g_logging.lock);
    if (path != g_logging.mutex_path || (!path.empty() && !g_logging.file_mutex)) {
        if (g_logging.file_mutex) {
            CloseHandle(g_logging.file_mutex);
            g_logging.file_mutex = nullptr;
        }
        g_logging.mutex_path = path;
        if (!path.empty()) {
            const std::wstring mutex_name = LogMutexName(path);
            g_logging.file_mutex = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
            if (!g_logging.file_mutex) {
                ReportInternalFailureLocked(L"mutex_create", GetLastError());
            }
        }
    }
    g_logging.enabled = enabled;
    g_logging.minimum_level = minimum_level;
    g_logging.path = std::move(path);
}

bool IsLogEnabled(LogLevel level) {
    ExclusiveLogLock lock(g_logging.lock);
    return g_logging.enabled && !g_logging.path.empty() && LevelEnabled(level);
}

void ResetLog() {
    ExclusiveLogLock lock(g_logging.lock);
    StartSessionLocked();
    if (!g_logging.enabled || g_logging.path.empty()) {
        return;
    }

    InterprocessLogLock file_lock(g_logging.file_mutex);
    if (!file_lock.acquired()) {
        ReportInternalFailureLocked(L"mutex_wait", file_lock.error());
        return;
    }

    if (!RotateLogLocked(true)) {
        return;
    }

    HANDLE file = CreateFileW(
        g_logging.path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ReportInternalFailureLocked(L"reset", GetLastError());
        return;
    }
    CloseHandle(file);
}

void LogDebug(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    LogMessage(LogLevel::Debug, format, args);
    va_end(args);
}

void LogInfo(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    LogMessage(LogLevel::Info, format, args);
    va_end(args);
}

void LogWarning(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    LogMessage(LogLevel::Warning, format, args);
    va_end(args);
}

void LogError(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    LogMessage(LogLevel::Error, format, args);
    va_end(args);
}

void LogWarningRateLimited(
    const wchar_t* key,
    unsigned interval_ms,
    const wchar_t* format,
    ...) {
    va_list args;
    va_start(args, format);
    LogRateLimitedMessage(LogLevel::Warning, key, interval_ms, format, args);
    va_end(args);
}

void LogErrorRateLimited(
    const wchar_t* key,
    unsigned interval_ms,
    const wchar_t* format,
    ...) {
    va_list args;
    va_start(args, format);
    LogRateLimitedMessage(LogLevel::Error, key, interval_ms, format, args);
    va_end(args);
}

void LogFailureRecovered(const wchar_t* key, const wchar_t* format, ...) {
    if (!key || *key == L'\0') {
        return;
    }

    ExclusiveLogLock lock(g_logging.lock);
    const auto found = g_logging.rate_limits.find(key);
    if (found == g_logging.rate_limits.end()) {
        return;
    }
    RateLimitEntry& entry = found->second;
    const auto now = SteadyClock::now();
    if (!entry.active) {
        if (entry.next_allowed == SteadyClock::time_point{} || now >= entry.next_allowed) {
            g_logging.rate_limits.erase(found);
        }
        return;
    }

    // A failure suppressed by the flap cooldown has no matching failure line,
    // so close it silently. Keep only the cooldown tombstone; carrying the
    // counters forward would include healthy time in a later failure period.
    if (!entry.failure_logged) {
        entry.level = LogLevel::Warning;
        entry.first_seen = SteadyClock::time_point{};
        entry.active = false;
        entry.total = 0;
        entry.pending_suppressed = 0;
        entry.total_suppressed = 0;
        entry.next_recovery_retry = SteadyClock::time_point{};
        return;
    }

    if (entry.next_recovery_retry != SteadyClock::time_point{} &&
        now < entry.next_recovery_retry) {
        return;
    }

    wchar_t message[2048]{};
    va_list args;
    va_start(args, format);
    const int format_result = format
        ? _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args)
        : 0;
    va_end(args);

    wchar_t combined[2560]{};
    _snwprintf_s(
        combined,
        _countof(combined),
        _TRUNCATE,
        L"%ls key=%ls total=%llu duration_ms=%lld pending_suppressed=%llu "
        L"suppressed_total=%llu",
        message,
        found->first.c_str(),
        static_cast<unsigned long long>(entry.total),
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - entry.first_seen).count(),
        static_cast<unsigned long long>(entry.pending_suppressed),
        static_cast<unsigned long long>(entry.total_suppressed));
    if (WriteMessageLocked(LogLevel::Info, combined, format_result < 0, 0, true)) {
        entry.level = LogLevel::Warning;
        entry.first_seen = SteadyClock::time_point{};
        entry.next_recovery_retry = SteadyClock::time_point{};
        entry.total = 0;
        entry.pending_suppressed = 0;
        entry.total_suppressed = 0;
        entry.active = false;
        entry.failure_logged = false;
    } else {
        entry.next_recovery_retry = now + kRecoveryRetryInterval;
    }
}

void FlushLogSummaries() {
    ExclusiveLogLock lock(g_logging.lock);
    const auto now = SteadyClock::now();
    const auto flush_entry = [&](const wchar_t* key, RateLimitEntry& entry) {
        if (!entry.active || entry.pending_suppressed == 0) {
            return;
        }

        wchar_t message[1024]{};
        _snwprintf_s(
            message,
            _countof(message),
            _TRUNCATE,
            L"event=repeat_summary key=%ls total=%llu duration_ms=%lld "
            L"pending_suppressed=%llu suppressed_total=%llu",
            key,
            static_cast<unsigned long long>(entry.total),
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - entry.first_seen).count(),
            static_cast<unsigned long long>(entry.pending_suppressed),
            static_cast<unsigned long long>(entry.total_suppressed));
        if (WriteMessageLocked(entry.level, message, false, 0, true)) {
            entry.pending_suppressed = 0;
        }
    };

    for (auto& item : g_logging.rate_limits) {
        flush_entry(item.first.c_str(), item.second);
    }
    flush_entry(L"rate_limit_overflow", g_logging.overflow_rate_limit);
    if (g_logging.overflow_rate_limit.pending_suppressed == 0) {
        g_logging.overflow_rate_limit.level = LogLevel::Warning;
    }
}

}  // namespace simple_monitor
