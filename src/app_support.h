#pragma once

#include <string>

namespace simple_monitor {

struct Config {
    int content_padding_x_dip = 8;
    int column_gap_dip = 28;
    int gap_after_network_dip = -1;
    int gap_after_system_dip = -1;
    int gap_after_disk_dip = 14;
    int offset_right_dip = 8;
    int font_size_dip = 13;
    int network_arrow_font_size_dip = 17;
    int network_arrow_gap_dip = 3;
    int key_font_size_dip = 13;
    bool show_key_widget = true;
    bool debug_log = false;
    std::wstring network_arrow_style = L"thin";
};

std::wstring ModulePath();
std::wstring ModuleDir();
std::wstring ConfigPath();
std::wstring DebugLogPath();

Config LoadConfig();

bool IsStartupEnabled(const wchar_t* run_value);
bool SetStartupEnabled(const wchar_t* run_value, bool enabled);

}  // namespace simple_monitor
