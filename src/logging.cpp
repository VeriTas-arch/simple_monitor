#include "logging.h"

#include <windows.h>

#include <cstdarg>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace simple_monitor {
namespace {

constexpr ULONGLONG kMaxLogBytes = 2ULL * 1024ULL * 1024ULL;

enum class LogLevel {
    Info,
    Warning,
    Error,
};

struct LoggingState {
    CRITICAL_SECTION lock{};
    bool enabled = false;
    std::wstring path;

    LoggingState() {
        InitializeCriticalSection(&lock);
    }

    ~LoggingState() {
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

const wchar_t* LogLevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Warning:
        return L"WARN";
    case LogLevel::Error:
        return L"ERROR";
    case LogLevel::Info:
    default:
        return L"INFO";
    }
}

bool FileSize(const std::wstring& path, ULONGLONG& size) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        return false;
    }

    ULARGE_INTEGER value{};
    value.HighPart = attributes.nFileSizeHigh;
    value.LowPart = attributes.nFileSizeLow;
    size = value.QuadPart;
    return true;
}

void RotateLogLocked(bool force) {
    if (g_logging.path.empty()) {
        return;
    }

    ULONGLONG size = 0;
    if (!FileSize(g_logging.path, size) || size == 0 || (!force && size < kMaxLogBytes)) {
        return;
    }

    const std::wstring backup_path = g_logging.path + L".1";
    DeleteFileW(backup_path.c_str());
    MoveFileExW(
        g_logging.path.c_str(),
        backup_path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void WriteLog(LogLevel level, const wchar_t* format, va_list args) {
    if (!format) {
        return;
    }

    wchar_t message[2048]{};
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);

    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t line[2560]{};
    _snwprintf_s(
        line,
        _countof(line),
        _TRUNCATE,
        L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%ls] [pid=%lu tid=%lu] %ls\r\n",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds,
        LogLevelName(level),
        GetCurrentProcessId(),
        GetCurrentThreadId(),
        message);

    ExclusiveLogLock lock(g_logging.lock);
    if (!g_logging.enabled || g_logging.path.empty()) {
        return;
    }

    RotateLogLocked(false);
    HANDLE file = CreateFileW(
        g_logging.path.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    const int line_chars = static_cast<int>(std::wcslen(line));
    const int byte_count = WideCharToMultiByte(CP_UTF8, 0, line, line_chars, nullptr, 0, nullptr, nullptr);
    if (byte_count > 0) {
        std::vector<char> buffer(static_cast<size_t>(byte_count));
        if (WideCharToMultiByte(CP_UTF8, 0, line, line_chars, buffer.data(), byte_count, nullptr, nullptr) > 0) {
            DWORD written = 0;
            WriteFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &written, nullptr);
        }
    }
    CloseHandle(file);
}

void LogMessage(LogLevel level, const wchar_t* format, va_list args) {
    WriteLog(level, format, args);
}

}  // namespace

void ConfigureLogging(bool enabled, std::wstring path) {
    ExclusiveLogLock lock(g_logging.lock);
    g_logging.enabled = enabled;
    g_logging.path = std::move(path);
}

void ResetLog() {
    ExclusiveLogLock lock(g_logging.lock);
    if (!g_logging.enabled || g_logging.path.empty()) {
        return;
    }

    RotateLogLocked(true);
    HANDLE file = CreateFileW(
        g_logging.path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
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

}  // namespace simple_monitor
