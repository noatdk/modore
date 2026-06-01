#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

namespace modore::windows {

struct ClipboardTimings {
    int pre_copy_delay_ms = 20;
    int read_timeout_ms = 250;
    int restore_clipboard_delay_ms = 50;
};

struct HotkeySpec {
    UINT modifiers = MOD_CONTROL;
    UINT vk = VK_OEM_1;
    std::wstring display_name = L"Ctrl+Semicolon";
    bool valid = true;
};

struct ConfigSnapshot {
    std::filesystem::path config_path;
    bool file_exists = false;
    bool hotkey_present = false;
    bool hotkey_valid = true;
    bool clipboard_valid = true;
    HotkeySpec hotkey = {};
    ClipboardTimings clipboard = {};
    std::vector<std::wstring> logging_disabled_roots;
    std::vector<std::wstring> issues;
};

std::filesystem::path config_dir();
std::filesystem::path config_file_path();
std::filesystem::path log_file_path();
std::filesystem::path executable_path();
std::filesystem::path mozc_profile_path();

HotkeySpec default_hotkey();
ConfigSnapshot load_config_snapshot();
ConfigSnapshot preflight_config();

std::wstring format_paths();

} // namespace modore::windows
