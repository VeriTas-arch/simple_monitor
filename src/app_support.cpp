#include "app_support.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace simple_monitor {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

int ReadConfigInt(
    const std::wstring& path,
    const wchar_t* key,
    int fallback,
    int min_value,
    int max_value) {
    const UINT raw = GetPrivateProfileIntW(L"layout", key, fallback, path.c_str());
    return std::clamp(static_cast<int>(raw), min_value, max_value);
}

bool ReadConfigBool(const std::wstring& path, const wchar_t* key, bool fallback) {
    return GetPrivateProfileIntW(L"layout", key, fallback ? 1 : 0, path.c_str()) != 0;
}

std::wstring ReadConfigString(
    const std::wstring& path,
    const wchar_t* key,
    const wchar_t* fallback) {
    wchar_t buffer[128]{};
    GetPrivateProfileStringW(L"layout", key, fallback, buffer, ARRAYSIZE(buffer), path.c_str());
    return buffer;
}

std::wstring LowerString(std::wstring value) {
    for (wchar_t& ch : value) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return value;
}

}  // namespace

std::wstring ModulePath() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (size == path.size()) {
        path.resize(path.size() * 2);
        size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    path.resize(size);
    return path;
}

std::wstring ModuleDir() {
    std::wstring path = ModulePath();
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        path.resize(slash);
    }
    return path;
}

std::wstring ConfigPath() {
    return ModuleDir() + L"\\simple_monitor.ini";
}

std::wstring DebugLogPath(const wchar_t* filename) {
    return ModuleDir() + L"\\" + (filename && *filename ? filename : L"debug.log");
}

Config LoadConfig() {
    const std::wstring path = ConfigPath();
    Config config;
    config.content_padding_x_dip = ReadConfigInt(path, L"content_padding_x", 8, 0, 80);
    config.column_gap_dip = ReadConfigInt(path, L"column_gap", 28, 0, 220);
    config.gap_after_network_dip = ReadConfigInt(path, L"gap_after_network", -1, -1, 220);
    config.gap_after_system_dip = ReadConfigInt(path, L"gap_after_system", -1, -1, 220);
    config.gap_after_disk_dip = ReadConfigInt(path, L"gap_after_disk", 14, 0, 220);
    config.offset_right_dip = ReadConfigInt(path, L"offset_right", 8, -200, 400);
    config.font_size_dip = ReadConfigInt(path, L"font_size", 13, 8, 28);
    config.network_arrow_font_size_dip = ReadConfigInt(path, L"network_arrow_font_size", 17, 8, 36);
    config.network_arrow_gap_dip = ReadConfigInt(path, L"network_arrow_gap", 3, 0, 20);
    config.key_font_size_dip = ReadConfigInt(path, L"key_font_size", config.font_size_dip, 8, 36);
    config.show_key_widget = ReadConfigBool(path, L"show_key_widget", true);
    config.debug_log = ReadConfigBool(path, L"debug_log", false);
    config.log_level = LowerString(ReadConfigString(path, L"log_level", L"info"));
    config.network_arrow_style = LowerString(ReadConfigString(path, L"network_arrow_style", L"thin"));
    if (config.log_level == L"warn") {
        config.log_level = L"warning";
    } else if (config.log_level != L"debug" &&
               config.log_level != L"info" &&
               config.log_level != L"warning" &&
               config.log_level != L"error") {
        config.log_level = L"info";
    }
    if (config.network_arrow_style != L"thin" &&
        config.network_arrow_style != L"triangle" &&
        config.network_arrow_style != L"heavy" &&
        config.network_arrow_style != L"chevron") {
        config.network_arrow_style = L"thin";
    }
    return config;
}

bool IsStartupEnabled(const wchar_t* run_value) {
    if (!run_value || *run_value == L'\0') {
        return false;
    }

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t value[2048]{};
    DWORD value_size = sizeof(value);
    DWORD type = 0;
    const LONG result = RegQueryValueExW(
        key,
        run_value,
        nullptr,
        &type,
        reinterpret_cast<LPBYTE>(value),
        &value_size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
}

bool SetStartupEnabled(const wchar_t* run_value, bool enabled) {
    if (!run_value || *run_value == L'\0') {
        return false;
    }

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }

    LONG result = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring command = L"\"" + ModulePath() + L"\" --startup";
        result = RegSetValueExW(
            key,
            run_value,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, run_value);
        if (result == ERROR_FILE_NOT_FOUND) {
            result = ERROR_SUCCESS;
        }
    }

    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

}  // namespace simple_monitor
