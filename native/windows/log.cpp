#include "log.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <windows.h>

namespace modore::windows {
namespace {

constexpr uint32_t bit_for(LogTag tag) {
    return 1u << static_cast<uint32_t>(tag);
}

std::wstring now_stamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::wostringstream out;
    out << std::setfill(L'0')
        << std::setw(4) << st.wYear << L'-'
        << std::setw(2) << st.wMonth << L'-'
        << std::setw(2) << st.wDay << L' '
        << std::setw(2) << st.wHour << L':'
        << std::setw(2) << st.wMinute << L':'
        << std::setw(2) << st.wSecond << L'.'
        << std::setw(3) << st.wMilliseconds;
    return out.str();
}

std::wstring utf8_to_wide(const std::wstring& s) {
    return s;
}

std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring root_of_tag(LogTag tag) {
    switch (tag) {
    case LogTag::Boot:
        return L"boot";
    case LogTag::Config:
        return L"config";
    case LogTag::Hotkey:
        return L"hotkey";
    case LogTag::Pickup:
        return L"pickup";
    case LogTag::Clipboard:
        return L"clipboard";
    case LogTag::Mozc:
        return L"mozc";
    case LogTag::SecureInput:
        return L"secure-input";
    case LogTag::Undo:
        return L"undo";
    case LogTag::Cycle:
        return L"cycle";
    case LogTag::Panel:
        return L"panel";
    case LogTag::Unicode:
        return L"unicode";
    case LogTag::Scripting:
        return L"scripting";
    }
    return L"";
}

void write_console_line(const std::wstring& line) {
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD mode = 0;
    if (GetFileType(handle) != FILE_TYPE_CHAR || !GetConsoleMode(handle, &mode)) {
        return;
    }
    DWORD written = 0;
    const std::wstring text = line + L"\n";
    WriteConsoleW(handle, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
}

} // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::set_log_path(const std::filesystem::path& path) {
    std::scoped_lock lock(mutex_);
    log_path_ = path;
}

std::filesystem::path Logger::log_path() const {
    std::scoped_lock lock(mutex_);
    return log_path_;
}

std::wstring join_disabled_roots(const std::vector<std::wstring>& roots) {
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

std::wstring escape_for_log(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size() + 2);
    for (wchar_t ch : text) {
        switch (ch) {
        case L'\\': out += L"\\\\"; break;
        case L'"': out += L"\\\""; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default:
            if (ch < 0x20) {
                out += L"\\x";
                const wchar_t hex[] = L"0123456789ABCDEF";
                out += hex[(ch >> 4) & 0xF];
                out += hex[ch & 0xF];
            } else {
                out.push_back(ch);
            }
        }
    }
    return out;
}

void Logger::configure_disabled_roots(const std::wstring& disabled) {
    uint32_t mask = 0;
    if (disabled == L"all") {
        mask = 0xFFFFFFFFu;
    }
    disabled_mask_.store(mask, std::memory_order_relaxed);
}

void Logger::configure_disabled_roots(const std::vector<std::wstring>& disabled) {
    uint32_t mask = 0;
    for (const auto& root : disabled) {
        const std::wstring r = root;
        if (r == L"all") {
            mask = 0xFFFFFFFFu;
            break;
        }
        if (r == L"boot") mask |= bit_for(LogTag::Boot);
        else if (r == L"config") mask |= bit_for(LogTag::Config);
        else if (r == L"hotkey") mask |= bit_for(LogTag::Hotkey);
        else if (r == L"pickup") mask |= bit_for(LogTag::Pickup);
        else if (r == L"clipboard") mask |= bit_for(LogTag::Clipboard);
        else if (r == L"mozc") mask |= bit_for(LogTag::Mozc);
        else if (r == L"secure-input") mask |= bit_for(LogTag::SecureInput);
        else if (r == L"undo") mask |= bit_for(LogTag::Undo);
        else if (r == L"cycle") mask |= bit_for(LogTag::Cycle);
        else if (r == L"panel") mask |= bit_for(LogTag::Panel);
        else if (r == L"unicode") mask |= bit_for(LogTag::Unicode);
        else if (r == L"scripting") mask |= bit_for(LogTag::Scripting);
    }
    disabled_mask_.store(mask, std::memory_order_relaxed);
}

bool Logger::is_disabled(LogTag tag) const {
    const uint32_t mask = disabled_mask_.load(std::memory_order_relaxed);
    return (mask & bit_for(tag)) != 0 || mask == 0xFFFFFFFFu;
}

std::wstring Logger::tag_name(LogTag tag) const {
    return root_of_tag(tag);
}

void Logger::write(LogTag tag, const std::wstring& message) {
    if (is_disabled(tag)) {
        return;
    }

    const std::wstring line = std::wstring(L"[") + tag_name(tag) + L"] " + message;
    OutputDebugStringW((line + L"\n").c_str());
    write_console_line(now_stamp() + L" " + line);

    std::filesystem::path path;
    {
        std::scoped_lock lock(mutex_);
        path = log_path_;
    }
    if (path.empty()) {
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    const std::string utf8 = wide_to_utf8(now_stamp() + L" " + line + L"\n");
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (out.is_open()) {
        out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }
}

} // namespace modore::windows
