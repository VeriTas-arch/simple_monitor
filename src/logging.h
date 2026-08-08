#pragma once

#include <string>

namespace simple_monitor {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
};

LogLevel ParseLogLevel(const std::wstring& value);
void ConfigureLogging(bool enabled, std::wstring path, LogLevel minimum_level = LogLevel::Info);
bool IsLogEnabled(LogLevel level = LogLevel::Info);
// Starts a new process session and rotates the previous session when possible.
void ResetLog();

void LogDebug(const wchar_t* format, ...);
void LogInfo(const wchar_t* format, ...);
void LogWarning(const wchar_t* format, ...);
void LogError(const wchar_t* format, ...);
// Rate-limit keys must be stable, low-cardinality identifiers, never dynamic handles or error text.
void LogWarningRateLimited(
    const wchar_t* key,
    unsigned interval_ms,
    const wchar_t* format,
    ...);
void LogErrorRateLimited(
    const wchar_t* key,
    unsigned interval_ms,
    const wchar_t* format,
    ...);
void LogFailureRecovered(const wchar_t* key, const wchar_t* format, ...);
void FlushLogSummaries();

}  // namespace simple_monitor
