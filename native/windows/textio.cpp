#include "textio.hpp"

#include <string>

#include <windows.h>
#include <UIAutomation.h>

namespace modore::windows {
namespace {

template <typename T>
void release_ptr(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

std::optional<std::wstring> text_from_range(IUIAutomationTextRange* range) {
    if (!range) {
        return std::nullopt;
    }

    BSTR text = nullptr;
    if (FAILED(range->GetText(-1, &text)) || !text) {
        return std::nullopt;
    }

    std::wstring result(text, SysStringLen(text));
    SysFreeString(text);
    return result;
}

std::optional<std::wstring> text_or_enclosing_word(IUIAutomationTextRange* range) {
    auto text = text_from_range(range);
    if (text && !text->empty()) {
        return text;
    }

    IUIAutomationTextRange* expanded = nullptr;
    if (FAILED(range->Clone(&expanded)) || !expanded) {
        return std::nullopt;
    }

    if (FAILED(expanded->ExpandToEnclosingUnit(TextUnit_Word))) {
        release_ptr(expanded);
        return std::nullopt;
    }

    text = text_from_range(expanded);
    if (!text || text->empty()) {
        release_ptr(expanded);
        return std::nullopt;
    }

    if (FAILED(expanded->Select())) {
        release_ptr(expanded);
        return std::nullopt;
    }

    release_ptr(expanded);
    return text;
}

} // namespace

std::optional<std::wstring> focused_selection_text() {
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool should_uninit = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        return std::nullopt;
    }

    IUIAutomation* automation = nullptr;
    IUIAutomationElement* focused = nullptr;
    IUIAutomationTextPattern* text_pattern = nullptr;
    IUIAutomationTextRangeArray* selection_ranges = nullptr;
    std::optional<std::wstring> selection;
    auto cleanup = [&]() {
        release_ptr(selection_ranges);
        release_ptr(text_pattern);
        release_ptr(focused);
        release_ptr(automation);
        if (should_uninit) {
            CoUninitialize();
        }
    };

    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) {
        cleanup();
        return std::nullopt;
    }

    if (FAILED(automation->GetFocusedElement(&focused)) || !focused) {
        cleanup();
        return std::nullopt;
    }

    if (FAILED(focused->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text_pattern))) || !text_pattern) {
        cleanup();
        return std::nullopt;
    }

    if (FAILED(text_pattern->GetSelection(&selection_ranges)) || !selection_ranges) {
        cleanup();
        return std::nullopt;
    }

    int count = 0;
    if (FAILED(selection_ranges->get_Length(&count)) || count <= 0) {
        cleanup();
        return std::nullopt;
    }

    for (int index = 0; index < count; ++index) {
        IUIAutomationTextRange* range = nullptr;
        if (FAILED(selection_ranges->GetElement(index, &range)) || !range) {
            continue;
        }

        selection = text_or_enclosing_word(range);
        release_ptr(range);
        if (selection && !selection->empty()) {
            break;
        }
        selection.reset();
    }

    cleanup();
    return selection;
}

} // namespace modore::windows
