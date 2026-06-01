#include "config.hpp"
#include "log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <shellapi.h>

using namespace std::chrono_literals;

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kMenuOpenConfig = 1001;
constexpr UINT kMenuRevealConfig = 1002;
constexpr UINT kMenuOpenLog = 1003;
constexpr UINT kMenuQuit = 1004;

struct RuntimeState {
    modore::windows::ConfigSnapshot config;
    std::wstring hotkey_label;
    bool hotkey_registered = false;
    bool running = true;
    HWND window = nullptr;
    NOTIFYICONDATAW nid{};
    std::mutex mutex;
};

RuntimeState g_state;
std::atomic<bool> g_reload_requested{false};
std::atomic<bool> g_stop_watcher{false};
std::thread g_watcher;

std::wstring wstring_from_path(const std::filesystem::path& p) {
    return p.wstring();
}

void show_message_box(const std::wstring& title, const std::wstring& message) {
    MessageBoxW(g_state.window, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
}

void open_path(const std::filesystem::path& path) {
    ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool register_hotkey(HWND window, const modore::windows::HotkeySpec& hotkey) {
    if (!RegisterHotKey(window, 1, hotkey.modifiers, hotkey.vk)) {
        return false;
    }
    return true;
}

void unregister_hotkey(HWND window) {
    UnregisterHotKey(window, 1);
}

void update_tray_tooltip(const std::wstring& text) {
    std::scoped_lock lock(g_state.mutex);
    wcsncpy_s(g_state.nid.szTip, text.c_str(), _TRUNCATE);
    g_state.nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_state.nid);
}

void refresh_hotkey_ui(const modore::windows::HotkeySpec& hotkey, bool registered) {
    g_state.hotkey_label = hotkey.display_name;
    const std::wstring tip = std::wstring(L"modore - ") + hotkey.display_name + (registered ? L" (running)" : L" (hotkey unavailable)");
    update_tray_tooltip(tip);
}

void apply_runtime_config(const modore::windows::ConfigSnapshot& snapshot) {
    auto& logger = modore::windows::Logger::instance();
    logger.set_log_path(modore::windows::log_file_path());
    logger.configure_disabled_roots(snapshot.logging_disabled_roots);

    if (!snapshot.issues.empty()) {
        for (const auto& issue : snapshot.issues) {
            logger.write(modore::windows::LogTag::Config, issue);
        }
    }

    bool hotkey_changed = false;
    {
        std::scoped_lock lock(g_state.mutex);
        hotkey_changed =
            snapshot.hotkey.modifiers != g_state.config.hotkey.modifiers ||
            snapshot.hotkey.vk != g_state.config.hotkey.vk;
    }

    if (hotkey_changed) {
        unregister_hotkey(g_state.window);
        const bool hotkey_ok = register_hotkey(g_state.window, snapshot.hotkey);
        {
            std::scoped_lock lock(g_state.mutex);
            if (hotkey_ok) {
                g_state.config.hotkey = snapshot.hotkey;
                g_state.hotkey_registered = true;
            } else {
                g_state.hotkey_registered = false;
            }
        }
        if (hotkey_ok) {
            logger.write(modore::windows::LogTag::Hotkey, std::wstring(L"config reloaded: hotkey=") + snapshot.hotkey.display_name);
            refresh_hotkey_ui(snapshot.hotkey, true);
        } else {
            logger.write(modore::windows::LogTag::Hotkey, std::wstring(L"config reload rejected: hotkey=") + snapshot.hotkey.display_name + L" (RegisterHotKey failed)");
            refresh_hotkey_ui(snapshot.hotkey, false);
        }
    }

    bool clipboard_changed = false;
    {
        std::scoped_lock lock(g_state.mutex);
        clipboard_changed =
            snapshot.clipboard.pre_copy_delay_ms != g_state.config.clipboard.pre_copy_delay_ms ||
            snapshot.clipboard.read_timeout_ms != g_state.config.clipboard.read_timeout_ms ||
            snapshot.clipboard.restore_clipboard_delay_ms != g_state.config.clipboard.restore_clipboard_delay_ms;
    }
    if (clipboard_changed) {
        {
            std::scoped_lock lock(g_state.mutex);
            g_state.config.clipboard = snapshot.clipboard;
        }
    logger.write(modore::windows::LogTag::Clipboard,
        std::wstring(L"clipboard timings: pre_copy=") + std::to_wstring(snapshot.clipboard.pre_copy_delay_ms) + L"ms"
        + L" read_timeout=" + std::to_wstring(snapshot.clipboard.read_timeout_ms) + L"ms"
        + L" restore=" + std::to_wstring(snapshot.clipboard.restore_clipboard_delay_ms) + L"ms");
    }

    {
        std::scoped_lock lock(g_state.mutex);
        g_state.config.logging_disabled_roots = snapshot.logging_disabled_roots;
    }
}

void request_reload() {
    g_reload_requested.store(true, std::memory_order_relaxed);
    if (g_state.window) {
        PostMessageW(g_state.window, WM_APP + 2, 0, 0);
    }
}

void watcher_thread() {
    auto last_write = std::filesystem::file_time_type::min();
    bool first = true;
    while (!g_stop_watcher.load(std::memory_order_relaxed)) {
        const auto path = modore::windows::config_file_path();
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        if (exists) {
            const auto now = std::filesystem::last_write_time(path, ec);
            if (first || now != last_write) {
                last_write = now;
                request_reload();
            }
        }
        first = false;
        std::this_thread::sleep_for(300ms);
    }
}

std::wstring current_executable_name() {
    const auto path = modore::windows::executable_path();
    if (path.empty()) {
        return L"modore-host.exe";
    }
    return path.filename().wstring();
}

void log_boot() {
    auto& logger = modore::windows::Logger::instance();
    logger.write(modore::windows::LogTag::Boot, std::wstring(L"pid=") + std::to_wstring(GetCurrentProcessId()));
    logger.write(modore::windows::LogTag::Boot, std::wstring(L"executable=") + current_executable_name());
    logger.write(modore::windows::LogTag::Boot, L"bundle path=not-applicable");
    logger.write(modore::windows::LogTag::Boot, std::wstring(L"config=") + modore::windows::config_file_path().wstring());
    logger.write(modore::windows::LogTag::Boot, std::wstring(L"log=") + modore::windows::log_file_path().wstring());
    logger.write(modore::windows::LogTag::Boot, std::wstring(L"mozc profile=") + modore::windows::mozc_profile_path().wstring());
}

void print_check_config() {
    const auto snapshot = modore::windows::preflight_config();
    std::wcout << L"config path: " << snapshot.config_path.wstring() << L"\n";
    if (!snapshot.file_exists) {
        std::wcout << L"  [conversion]  default [conversion] hotkey not set in " << snapshot.config_path.wstring() << L"\n";
    } else if (!snapshot.hotkey_valid) {
        std::wcout << L"  [conversion]  INVALID malformed [conversion] hotkey in " << snapshot.config_path.wstring() << L"\n";
    } else if (snapshot.hotkey_present) {
        std::wcout << L"  [conversion]  ok      [conversion] hotkey=" << snapshot.hotkey.display_name << L" (" << snapshot.config_path.wstring() << L")\n";
    } else {
        std::wcout << L"  [conversion]  default [conversion] hotkey not set in " << snapshot.config_path.wstring() << L"\n";
    }

    std::wcout << L"  [clipboard]   pre_copy=" << snapshot.clipboard.pre_copy_delay_ms
               << L"ms read_timeout=" << snapshot.clipboard.read_timeout_ms
               << L"ms restore=" << snapshot.clipboard.restore_clipboard_delay_ms << L"ms\n";
    std::wcout << L"  [logging]     disabled=" << modore::windows::join_disabled_roots(snapshot.logging_disabled_roots) << L"\n";
    for (const auto& issue : snapshot.issues) {
        std::wcout << L"                " << issue << L"\n";
    }
}

void print_paths() {
    std::wcout << modore::windows::format_paths() << L"\n";
}

void create_tray_icon(HWND window) {
    g_state.nid = {};
    g_state.nid.cbSize = sizeof(g_state.nid);
    g_state.nid.hWnd = window;
    g_state.nid.uID = 1;
    g_state.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_state.nid.uCallbackMessage = kTrayMessage;
    g_state.nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(g_state.nid.szTip, L"modore", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_state.nid);
}

void destroy_tray_icon() {
    Shell_NotifyIconW(NIM_DELETE, &g_state.nid);
}

void show_tray_menu(HWND window) {
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(window);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuOpenConfig, L"Open config");
    AppendMenuW(menu, MF_STRING, kMenuRevealConfig, L"Reveal config folder");
    AppendMenuW(menu, MF_STRING, kMenuOpenLog, L"Open log");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit modore");
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, window, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        create_tray_icon(window);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kMenuOpenConfig:
            open_path(modore::windows::config_file_path());
            return 0;
        case kMenuRevealConfig:
            open_path(modore::windows::config_dir());
            return 0;
        case kMenuOpenLog:
            open_path(modore::windows::log_file_path());
            return 0;
        case kMenuQuit:
            DestroyWindow(window);
            return 0;
        default:
            break;
        }
        break;
    case WM_HOTKEY:
        modore::windows::Logger::instance().write(modore::windows::LogTag::Hotkey, L"conversion hotkey fired");
        return 0;
    case WM_APP + 2:
        if (g_reload_requested.exchange(false, std::memory_order_relaxed)) {
            auto next = modore::windows::load_config_snapshot();
            apply_runtime_config(next);
        }
        return 0;
    case kTrayMessage:
        if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
            show_tray_menu(window);
        } else if (lparam == WM_LBUTTONUP) {
            show_message_box(L"modore", L"modore is running.");
        }
        return 0;
    case WM_DESTROY:
        destroy_tray_icon();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, msg, wparam, lparam);
    }
    return 0;
}

int run_host() {
    const auto snapshot = modore::windows::load_config_snapshot();
    g_state.config = snapshot;

    auto& logger = modore::windows::Logger::instance();
    logger.set_log_path(modore::windows::log_file_path());
    logger.configure_disabled_roots(snapshot.logging_disabled_roots);

    log_boot();

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* class_name = L"modore_host_window";
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    RegisterClassW(&wc);

    HWND window = CreateWindowExW(
        0, class_name, L"", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        logger.write(modore::windows::LogTag::Boot, L"failed to create window");
        return 1;
    }
    g_state.window = window;

    if (!register_hotkey(window, snapshot.hotkey)) {
        logger.write(modore::windows::LogTag::Hotkey, std::wstring(L"hotkey registration failed for ") + snapshot.hotkey.display_name);
        refresh_hotkey_ui(snapshot.hotkey, false);
    } else {
        g_state.hotkey_registered = true;
        refresh_hotkey_ui(snapshot.hotkey, true);
        logger.write(modore::windows::LogTag::Hotkey, std::wstring(L"hotkey registered: ") + snapshot.hotkey.display_name);
    }

    logger.write(modore::windows::LogTag::Clipboard,
        std::wstring(L"clipboard timings: pre_copy=") + std::to_wstring(snapshot.clipboard.pre_copy_delay_ms) + L"ms"
        + L" read_timeout=" + std::to_wstring(snapshot.clipboard.read_timeout_ms) + L"ms"
        + L" restore=" + std::to_wstring(snapshot.clipboard.restore_clipboard_delay_ms) + L"ms");

    ShowWindow(window, SW_HIDE);

    g_stop_watcher.store(false, std::memory_order_relaxed);
    g_watcher = std::thread(watcher_thread);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_stop_watcher.store(true, std::memory_order_relaxed);
    if (g_watcher.joinable()) {
        g_watcher.join();
    }
    unregister_hotkey(window);
    return 0;
}

int run_with_args(const std::vector<std::wstring>& args) {
    if (std::find(args.begin(), args.end(), L"--print-config-path") != args.end()) {
        std::wcout << modore::windows::config_file_path().wstring() << L"\n";
        return 0;
    }
    if (std::find(args.begin(), args.end(), L"--print-paths") != args.end()) {
        print_paths();
        return 0;
    }
    if (std::find(args.begin(), args.end(), L"--check-config") != args.end()) {
        const auto snapshot = modore::windows::preflight_config();
        print_check_config();
        if (!snapshot.hotkey_valid) {
            return 1;
        }
        if (!snapshot.clipboard_valid) {
            return 2;
        }
        return 0;
    }

    return run_host();
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return 1;
    }

    std::vector<std::wstring> args;
    args.reserve(argc > 0 ? static_cast<size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    LocalFree(argv);
    return run_with_args(args);
}
