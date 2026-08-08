#pragma once

#include <string>

namespace simple_monitor {

void ConfigureLogging(bool enabled, std::wstring path);
void ResetLog();

void LogInfo(const wchar_t* format, ...);
void LogWarning(const wchar_t* format, ...);
void LogError(const wchar_t* format, ...);

}  // namespace simple_monitor
