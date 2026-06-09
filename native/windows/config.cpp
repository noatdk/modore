#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include <shlobj.h>
#include <windows.h>

namespace modore::windows {
namespace {

constexpr int kClipboardFields = 3;

std::wstring trim(std::wstring s) {
    auto is_space = [](wchar_t ch) {
        return ::iswspace(ch) != 0;
    };
    auto start = std::find_if_not(s.begin(), s.end(), is_space);
    auto end = std::find_if_not(s.rbegin(), s.rend(), is_space).base();
    if (start >= end) {
        return L"";
    }
    return std::wstring(start, end);
}

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(::towlower(ch));
    });
    return s;
}

std::vector<std::wstring> split(std::wstring_view text, wchar_t sep) {
    std::vector<std::wstring> out;
    std::wstring current;
    for (wchar_t ch : text) {
        if (ch == sep) {
            out.push_back(trim(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    out.push_back(trim(current));
    return out;
}

std::optional<int> parse_int(std::wstring_view text) {
    try {
        size_t idx = 0;
        const int value = std::stoi(std::wstring(text), &idx);
        if (idx != text.size()) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

struct ParsedHotkey {
    UINT modifiers = 0;
    UINT vk = 0;
    std::wstring display_name;
    bool valid = false;
};

std::wstring canonical_mod_name(UINT modifier) {
    switch (modifier) {
    case MOD_CONTROL:
        return L"Ctrl";
    case MOD_SHIFT:
        return L"Shift";
    case MOD_ALT:
        return L"Alt";
    case MOD_WIN:
        return L"Win";
    default:
        return L"";
    }
}

std::wstring vk_display_name(UINT vk) {
    switch (vk) {
    case VK_OEM_1:
        return L"Semicolon";
    case VK_OEM_3:
        return L"grave";
    case VK_OEM_COMMA:
        return L"Comma";
    case VK_OEM_PERIOD:
        return L"Period";
    case VK_OEM_2:
        return L"Slash";
    case VK_OEM_5:
        return L"Backslash";
    case VK_SPACE:
        return L"Space";
    case VK_TAB:
        return L"Tab";
    case VK_RETURN:
        return L"Return";
    case VK_ESCAPE:
        return L"Escape";
    case VK_BACK:
        return L"Backspace";
    case VK_LEFT:
        return L"Left";
    case VK_RIGHT:
        return L"Right";
    case VK_UP:
        return L"Up";
    case VK_DOWN:
        return L"Down";
    case VK_HOME:
        return L"Home";
    case VK_END:
        return L"End";
    case VK_PRIOR:
        return L"PageUp";
    case VK_NEXT:
        return L"PageDown";
    case VK_INSERT:
        return L"Insert";
    default:
        if (vk >= 'A' && vk <= 'Z') {
            return std::wstring(1, static_cast<wchar_t>(vk));
        }
        if (vk >= '0' && vk <= '9') {
            return std::wstring(1, static_cast<wchar_t>(vk));
        }
        if (vk >= VK_F1 && vk <= VK_F24) {
            return std::wstring(L"F") + std::to_wstring(vk - VK_F1 + 1);
        }
        return L"";
    }
}

std::optional<UINT> parse_vk_token(const std::wstring& token) {
    const std::wstring t = lower(trim(token));
    if (t.size() == 1) {
        const wchar_t ch = t[0];
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9')) {
            return static_cast<UINT>(::towupper(ch));
        }
    }

    static const std::unordered_map<std::wstring, UINT> kMap = {
        {L"semicolon", VK_OEM_1},
        {L"grave", VK_OEM_3},
        {L"backtick", VK_OEM_3},
        {L"comma", VK_OEM_COMMA},
        {L"period", VK_OEM_PERIOD},
        {L"dot", VK_OEM_PERIOD},
        {L"slash", VK_OEM_2},
        {L"backslash", VK_OEM_5},
        {L"space", VK_SPACE},
        {L"tab", VK_TAB},
        {L"return", VK_RETURN},
        {L"enter", VK_RETURN},
        {L"escape", VK_ESCAPE},
        {L"esc", VK_ESCAPE},
        {L"backspace", VK_BACK},
        {L"left", VK_LEFT},
        {L"right", VK_RIGHT},
        {L"up", VK_UP},
        {L"down", VK_DOWN},
        {L"home", VK_HOME},
        {L"end", VK_END},
        {L"pageup", VK_PRIOR},
        {L"pagedown", VK_NEXT},
        {L"insert", VK_INSERT},
        {L"delete", VK_DELETE},
    };

    if (auto it = kMap.find(t); it != kMap.end()) {
        return it->second;
    }

    if (t.size() >= 2 && t[0] == L'f') {
        const auto suffix = parse_int(t.substr(1));
        if (suffix && *suffix >= 1 && *suffix <= 24) {
            return static_cast<UINT>(VK_F1 + *suffix - 1);
        }
    }

    return std::nullopt;
}

std::optional<UINT> parse_modifier_token(const std::wstring& token) {
    const std::wstring t = lower(trim(token));
    if (t == L"ctrl" || t == L"control") {
        return MOD_CONTROL;
    }
    if (t == L"shift") {
        return MOD_SHIFT;
    }
    if (t == L"alt" || t == L"option") {
        return MOD_ALT;
    }
    if (t == L"meta" || t == L"super" || t == L"win" || t == L"command" || t == L"cmd") {
        return MOD_WIN;
    }
    return std::nullopt;
}

ParsedHotkey parse_hotkey(std::wstring_view value) {
    ParsedHotkey out;
    auto tokens = split(value, L'+');
    if (tokens.size() < 2) {
        return out;
    }

    UINT modifiers = 0;
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        auto modifier = parse_modifier_token(tokens[i]);
        if (!modifier) {
            return out;
        }
        modifiers |= *modifier;
    }

    auto vk = parse_vk_token(tokens.back());
    if (!vk) {
        return out;
    }

    out.modifiers = modifiers;
    out.vk = *vk;
    out.valid = true;

    std::wstring display;
    bool first = true;
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        auto modifier = parse_modifier_token(tokens[i]);
        if (!modifier) {
            return {};
        }
        if (!first) {
            display += L"+";
        }
        display += canonical_mod_name(*modifier);
        first = false;
    }
    if (!display.empty()) {
        display += L"+";
    }
    display += vk_display_name(*vk);
    out.display_name = display;
    return out;
}

std::wstring join_roots(const std::vector<std::wstring>& roots) {
    if (roots.empty()) {
        return L"none";
    }
    std::wstring out;
    for (size_t i = 0; i < roots.size(); ++i) {
        if (i) {
            out += L",";
        }
        out += roots[i];
    }
    return out;
}

std::filesystem::path known_folder_path(const KNOWNFOLDERID& id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) {
        return {};
    }
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

std::vector<std::wstring> parse_logging_disabled(std::wistream& in) {
    std::vector<std::wstring> disabled;
    std::wstring line;
    std::wstring section;
    while (std::getline(in, line)) {
        std::wstring trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == L'#' || trimmed[0] == L';') {
            continue;
        }
        if (trimmed.front() == L'[' && trimmed.back() == L']') {
            section = lower(trim(trimmed.substr(1, trimmed.size() - 2)));
            continue;
        }
        const auto eq = trimmed.find(L'=');
        if (eq == std::wstring::npos) {
            continue;
        }
        if (section != L"logging") {
            continue;
        }
        const std::wstring key = lower(trim(trimmed.substr(0, eq)));
        if (key != L"disabled") {
            continue;
        }
        const std::wstring value = trim(trimmed.substr(eq + 1));
        for (const auto& token : split(value, L',')) {
            if (token.empty()) {
                continue;
            }
            const std::wstring t = lower(token);
            if (t == L"none") {
                disabled.clear();
                continue;
            }
            if (t == L"all") {
                return {L"all"};
            }
            disabled.push_back(t);
        }
    }
    return disabled;
}

std::wstring config_text_path(const std::filesystem::path& path) {
    return path.wstring();
}

} // namespace

std::filesystem::path config_dir() {
    auto base = known_folder_path(FOLDERID_RoamingAppData);
    if (base.empty()) {
        base = known_folder_path(FOLDERID_LocalAppData);
    }
    if (base.empty()) {
        return L".";
    }
    return base / L"modore";
}

std::filesystem::path config_file_path() {
    return config_dir() / L"modore.conf";
}

std::filesystem::path log_file_path() {
    return config_dir() / L"modore.log";
}

std::filesystem::path ime_profile_path() {
    return config_dir() / L"mozc";
}

std::filesystem::path executable_path() {
    wchar_t buffer[MAX_PATH + 1] = {};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len > MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buffer);
}

HotkeySpec default_hotkey() {
    return {};
}

ConfigSnapshot load_config_snapshot() {
    ConfigSnapshot snapshot;
    snapshot.config_path = config_file_path();
    snapshot.file_exists = std::filesystem::exists(snapshot.config_path);
    snapshot.hotkey = default_hotkey();

    if (!snapshot.file_exists) {
        snapshot.hotkey_present = false;
        snapshot.hotkey_valid = true;
        snapshot.clipboard_valid = true;
        return snapshot;
    }

    std::wifstream file(snapshot.config_path);
    if (!file.is_open()) {
        snapshot.issues.push_back(L"could not open config file");
        snapshot.hotkey_valid = false;
        snapshot.clipboard_valid = false;
        return snapshot;
    }

    std::wstring section;
    std::wstring line;
    while (std::getline(file, line)) {
        std::wstring trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == L'#' || trimmed[0] == L';') {
            continue;
        }
        if (trimmed.front() == L'[' && trimmed.back() == L']') {
            section = lower(trim(trimmed.substr(1, trimmed.size() - 2)));
            continue;
        }
        const auto eq = trimmed.find(L'=');
        if (eq == std::wstring::npos) {
            continue;
        }

        const std::wstring key = lower(trim(trimmed.substr(0, eq)));
        const std::wstring value = trim(trimmed.substr(eq + 1));

        if (section == L"conversion" && key == L"hotkey") {
            snapshot.hotkey_present = true;
            auto parsed = parse_hotkey(value);
            if (parsed.valid) {
                snapshot.hotkey = {parsed.modifiers, parsed.vk, parsed.display_name, true};
                snapshot.hotkey_valid = true;
            } else {
                snapshot.hotkey_valid = false;
                snapshot.issues.push_back(
                    std::wstring(L"malformed [conversion] hotkey=") + value + L" in " + config_text_path(snapshot.config_path));
                snapshot.hotkey = default_hotkey();
            }
            continue;
        }

        if (section == L"clipboard") {
            const auto parsed = parse_int(value);
            if (key == L"pre_copy_delay_ms") {
                if (parsed && *parsed >= 0) {
                    snapshot.clipboard.pre_copy_delay_ms = *parsed;
                } else {
                    snapshot.clipboard_valid = false;
                    snapshot.issues.push_back(
                        std::wstring(L"ignoring [clipboard] pre_copy_delay_ms=") + value + L" (expected non-negative integer)");
                }
            } else if (key == L"read_timeout_ms") {
                if (parsed && *parsed >= 0) {
                    snapshot.clipboard.read_timeout_ms = *parsed;
                } else {
                    snapshot.clipboard_valid = false;
                    snapshot.issues.push_back(
                        std::wstring(L"ignoring [clipboard] read_timeout_ms=") + value + L" (expected non-negative integer)");
                }
            } else if (key == L"restore_clipboard_delay_ms") {
                if (parsed && *parsed >= 0) {
                    snapshot.clipboard.restore_clipboard_delay_ms = *parsed;
                } else {
                    snapshot.clipboard_valid = false;
                    snapshot.issues.push_back(
                        std::wstring(L"ignoring [clipboard] restore_clipboard_delay_ms=") + value + L" (expected non-negative integer)");
                }
            }
            continue;
        }

        if (section == L"logging" && key == L"disabled") {
            std::wistringstream value_stream(value);
            auto disabled = parse_logging_disabled(value_stream);
            if (!disabled.empty()) {
                snapshot.logging_disabled_roots = std::move(disabled);
            }
            continue;
        }
    }

    return snapshot;
}

ConfigSnapshot preflight_config() {
    return load_config_snapshot();
}

std::wstring format_paths() {
    const auto config = config_file_path().wstring();
    const auto log = log_file_path().wstring();
    const auto profile = ime_profile_path().wstring();
    const auto exe = executable_path().wstring();
    std::wostringstream out;
    out << L"config:        " << config << L"\n";
    out << L"log:           " << log << L"\n";
    out << L"ime profile:   " << profile << L"\n";
    out << L"executable:    " << exe;
    return out.str();
}

} // namespace modore::windows
