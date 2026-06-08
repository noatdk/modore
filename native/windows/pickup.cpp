#include "pickup.hpp"
#include "cycle.hpp"
#include "textio.hpp"

#include <chrono>
#include <cstring>
#include <utility>
#include <optional>
#include <string>
#include <thread>

#include <windows.h>

namespace modore::windows {
namespace {

struct ClipboardSnapshot {
    std::wstring text;
    bool has_text = false;
};

using Clock = std::chrono::steady_clock;

long long elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

std::optional<std::wstring> replace_unique_occurrence(
    const std::wstring& text,
    const std::wstring& needle,
    const std::wstring& replacement) {
    if (needle.empty()) {
        return std::nullopt;
    }
    const size_t first = text.find(needle);
    if (first == std::wstring::npos) {
        return std::nullopt;
    }
    if (text.find(needle, first + needle.size()) != std::wstring::npos) {
        return std::nullopt;
    }
    std::wstring updated = text;
    updated.replace(first, needle.size(), replacement);
    return updated;
}

std::wstring conversion_destination_label(const PickupConversionResult& conversion) {
    const size_t total = conversion.candidates.empty() ? 1 : conversion.candidates.size();
    return std::wstring(L"\"") + escape_for_log(conversion.committed) + L"\" [1/" + std::to_wstring(total) + L"]";
}

bool open_clipboard_for(HWND owner) {
    return OpenClipboard(owner) != 0;
}

ClipboardSnapshot read_clipboard_text(HWND owner) {
    ClipboardSnapshot snapshot;
    if (!open_clipboard_for(owner)) {
        return snapshot;
    }
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle) {
        const auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
        if (text) {
            snapshot.text = text;
            snapshot.has_text = true;
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return snapshot;
}

bool write_clipboard_text(HWND owner, const std::wstring& text) {
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!handle) {
        return false;
    }

    void* raw = GlobalLock(handle);
    if (!raw) {
        GlobalFree(handle);
        return false;
    }

    std::memcpy(raw, text.c_str(), bytes);
    GlobalUnlock(handle);

    if (!open_clipboard_for(owner)) {
        GlobalFree(handle);
        return false;
    }

    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, handle)) {
        GlobalFree(handle);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

void restore_clipboard_later(HWND owner, std::wstring text, int delay_ms) {
    if (delay_ms <= 0) {
        write_clipboard_text(owner, text);
        return;
    }

    std::thread([owner, text = std::move(text), delay_ms]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        write_clipboard_text(owner, text);
    }).detach();
}

void send_key_combo(WORD vk) {
    INPUT inputs[4]{};

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;

    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = vk;
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;

    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(4, inputs, sizeof(INPUT));
}

void send_key(WORD vk) {
    INPUT inputs[2]{};

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(2, inputs, sizeof(INPUT));
}

void send_select_previous_word() {
    INPUT inputs[6]{};

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_SHIFT;

    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = VK_LEFT;

    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_LEFT;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    inputs[4].type = INPUT_KEYBOARD;
    inputs[4].ki.wVk = VK_SHIFT;
    inputs[4].ki.dwFlags = KEYEVENTF_KEYUP;

    inputs[5].type = INPUT_KEYBOARD;
    inputs[5].ki.wVk = VK_CONTROL;
    inputs[5].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(6, inputs, sizeof(INPUT));
}

std::optional<std::wstring> poll_clipboard_change(HWND owner, DWORD baseline_sequence, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (GetClipboardSequenceNumber() != baseline_sequence) {
            auto snapshot = read_clipboard_text(owner);
            if (snapshot.has_text && !snapshot.text.empty()) {
                return snapshot.text;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return std::nullopt;
}

std::optional<std::wstring> wait_for_selection(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto selection = focused_selection_text();
        if (selection && !selection->empty()) {
            return selection;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return std::nullopt;
}

} // namespace

bool perform_pickup(const ConfigSnapshot& config, Logger& logger, const PickupConverter& convert) {
    const auto trigger_at = Clock::now();
    const HWND owner = nullptr;
    bool did_force_select = false;
    std::optional<std::wstring> clipboard_restore_text;
    Clock::time_point picked_at = trigger_at;
    Clock::time_point converted_at = trigger_at;
    Clock::time_point pasted_at = trigger_at;

    reset_cycle();

    std::optional<std::wstring> picked = focused_selection_text();
    if (picked && !picked->empty()) {
        logger.write(LogTag::Pickup, std::wstring(L"pickup captured selection via UIA bytes=") + std::to_wstring(picked->size()));
    } else {
        picked.reset();
        send_select_previous_word();
        did_force_select = true;
        picked = wait_for_selection(config.clipboard.pre_copy_delay_ms);
        if (picked) {
            logger.write(LogTag::Pickup, std::wstring(L"pickup captured selection after force-select via UIA bytes=") + std::to_wstring(picked->size()));
        } else {
            const auto clipboard_before = read_clipboard_text(owner);
            const DWORD baseline_sequence = GetClipboardSequenceNumber();
            send_key_combo('C');

            picked = poll_clipboard_change(owner, baseline_sequence, config.clipboard.read_timeout_ms);
            if (picked) {
                logger.write(LogTag::Pickup, std::wstring(L"pickup captured clipboard bytes=") + std::to_wstring(picked->size()));
            }

            if (clipboard_before.has_text) {
                clipboard_restore_text = clipboard_before.text;
            }
        }
    }

    picked_at = Clock::now();

    if (!picked) {
        logger.write(LogTag::Pickup, L"pickup failed: no selection via UIA and clipboard did not change after Ctrl+Shift+Left/Ctrl+C");
        if (did_force_select) {
            send_key(VK_RIGHT);
        }
        if (clipboard_restore_text) {
            write_clipboard_text(owner, *clipboard_restore_text);
        }
        return false;
    }

    logger.write(
        LogTag::Pickup,
        std::wstring(L"pickup text=\"") + escape_for_log(*picked) + L"\" bytes=" + std::to_wstring(picked->size()));

    const auto field_before_replace = focused_editable_text();
    const auto convert_started = Clock::now();
    if (auto conversion = convert(*picked)) {
        converted_at = Clock::now();
        const auto native_started = Clock::now();
        if (replace_focused_selection_text(*picked, conversion->committed)) {
            pasted_at = Clock::now();
            logger.write(
                LogTag::Pickup,
                std::wstring(L"pickup replaced text via native API to=") + conversion_destination_label(*conversion) +
                L" total_ms=" + std::to_wstring(elapsed_ms(trigger_at, pasted_at)) +
                L" selection_ms=" + std::to_wstring(elapsed_ms(trigger_at, picked_at)) +
                L" convert_ms=" + std::to_wstring(elapsed_ms(convert_started, converted_at)) +
                L" native_ms=" + std::to_wstring(elapsed_ms(native_started, pasted_at)));
            if (field_before_replace) {
                if (auto updated = replace_unique_occurrence(*field_before_replace, *picked, conversion->committed)) {
                    if (conversion->candidates.size() > 1) {
                        remember_cycle(*updated, *conversion);
                    } else {
                        reset_cycle();
                    }
                }
            }
        } else {
            if (!clipboard_restore_text) {
                const auto clipboard_before = read_clipboard_text(owner);
                if (clipboard_before.has_text) {
                    clipboard_restore_text = std::move(clipboard_before.text);
                }
            }
            if (write_clipboard_text(owner, conversion->committed)) {
                send_key_combo('V');
                pasted_at = Clock::now();
                logger.write(
                    LogTag::Pickup,
                    std::wstring(L"pickup replaced text to=") + conversion_destination_label(*conversion) +
                    L" total_ms=" + std::to_wstring(elapsed_ms(trigger_at, pasted_at)) +
                    L" selection_ms=" + std::to_wstring(elapsed_ms(trigger_at, picked_at)) +
                    L" convert_ms=" + std::to_wstring(elapsed_ms(convert_started, converted_at)) +
                    L" paste_ms=" + std::to_wstring(elapsed_ms(converted_at, pasted_at)));
                if (clipboard_restore_text) {
                    restore_clipboard_later(owner, std::move(*clipboard_restore_text), config.clipboard.restore_clipboard_delay_ms);
                }
                if (field_before_replace) {
                    if (auto updated = replace_unique_occurrence(*field_before_replace, *picked, conversion->committed)) {
                        if (conversion->candidates.size() > 1) {
                            remember_cycle(*updated, *conversion);
                        } else {
                            reset_cycle();
                        }
                    }
                }
            } else {
                logger.write(LogTag::Pickup, L"pickup replacement skipped: could not write replacement to clipboard");
                if (clipboard_restore_text) {
                    write_clipboard_text(owner, *clipboard_restore_text);
                }
            }
        }
    } else {
        logger.write(LogTag::Pickup, L"pickup replacement skipped: converter returned no replacement");
        if (clipboard_restore_text) {
            write_clipboard_text(owner, *clipboard_restore_text);
        }
    }
    return true;
}

} // namespace modore::windows
