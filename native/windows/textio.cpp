#include "textio.hpp"

#include <string>

#include <windows.h>
#include <objbase.h>
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

std::optional<std::wstring> text_from_value_pattern(IUIAutomationValuePattern* value_pattern) {
    if (!value_pattern) {
        return std::nullopt;
    }

    BSTR value = nullptr;
    if (FAILED(value_pattern->get_CurrentValue(&value)) || !value) {
        return std::nullopt;
    }

    std::wstring result(value, SysStringLen(value));
    SysFreeString(value);
    return result;
}

std::optional<std::wstring> current_editable_text(IUIAutomationElement* focused) {
    if (!focused) {
        return std::nullopt;
    }

    IUIAutomationValuePattern* value_pattern = nullptr;
    if (SUCCEEDED(focused->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value_pattern))) && value_pattern) {
        auto text = text_from_value_pattern(value_pattern);
        release_ptr(value_pattern);
        if (text && !text->empty()) {
            return text;
        }
    }

    IUIAutomationTextPattern* text_pattern = nullptr;
    if (FAILED(focused->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text_pattern))) || !text_pattern) {
        return std::nullopt;
    }

    IUIAutomationTextRange* document_range = nullptr;
    if (FAILED(text_pattern->get_DocumentRange(&document_range)) || !document_range) {
        release_ptr(text_pattern);
        return std::nullopt;
    }

    auto text = text_from_range(document_range);
    release_ptr(document_range);
    release_ptr(text_pattern);
    if (text && !text->empty()) {
        return text;
    }
    return std::nullopt;
}

bool set_caret_after_offset(IUIAutomationElement* focused, int offset) {
    if (!focused || offset < 0) {
        return false;
    }

    IUIAutomationTextPattern* text_pattern = nullptr;
    if (FAILED(focused->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text_pattern))) || !text_pattern) {
        return false;
    }

    IUIAutomationTextRange* document_range = nullptr;
    if (FAILED(text_pattern->get_DocumentRange(&document_range)) || !document_range) {
        release_ptr(text_pattern);
        return false;
    }

    bool success = false;
    int moved = 0;
    if (offset > 0) {
        if (FAILED(document_range->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character, offset, &moved)) || moved != offset) {
            release_ptr(document_range);
            release_ptr(text_pattern);
            return false;
        }
    }

    if (FAILED(document_range->MoveEndpointByRange(TextPatternRangeEndpoint_End, document_range, TextPatternRangeEndpoint_Start))) {
        release_ptr(document_range);
        release_ptr(text_pattern);
        return false;
    }

    success = SUCCEEDED(document_range->Select());
    release_ptr(document_range);
    release_ptr(text_pattern);
    return success;
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

std::optional<std::wstring> focused_editable_text() {
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool should_uninit = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        return std::nullopt;
    }

    IUIAutomation* automation = nullptr;
    IUIAutomationElement* focused = nullptr;
    std::optional<std::wstring> text;
    auto cleanup = [&]() {
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

    text = current_editable_text(focused);
    cleanup();
    return text;
}

bool replace_focused_selection_text(const std::wstring& selected_text, const std::wstring& replacement) {
    if (selected_text.empty()) {
        return false;
    }

    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool should_uninit = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        return false;
    }

    IUIAutomation* automation = nullptr;
    IUIAutomationElement* focused = nullptr;
    IUIAutomationValuePattern* value_pattern = nullptr;
    bool success = false;
    auto cleanup = [&]() {
        release_ptr(value_pattern);
        release_ptr(focused);
        release_ptr(automation);
        if (should_uninit) {
            CoUninitialize();
        }
    };

    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) {
        cleanup();
        return false;
    }

    if (FAILED(automation->GetFocusedElement(&focused)) || !focused) {
        cleanup();
        return false;
    }

    if (FAILED(focused->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value_pattern))) || !value_pattern) {
        cleanup();
        return false;
    }

    auto current_text = current_editable_text(focused);
    if (!current_text || current_text->empty()) {
        cleanup();
        return false;
    }

    const size_t first = current_text->find(selected_text);
    if (first == std::wstring::npos) {
        cleanup();
        return false;
    }

    const size_t second = current_text->find(selected_text, first + selected_text.size());
    if (second != std::wstring::npos) {
        cleanup();
        return false;
    }

    std::wstring updated = *current_text;
    updated.replace(first, selected_text.size(), replacement);
    BSTR updated_bstr = SysAllocStringLen(updated.data(), static_cast<UINT>(updated.size()));
    if (!updated_bstr) {
        cleanup();
        return false;
    }
    success = SUCCEEDED(value_pattern->SetValue(updated_bstr));
    SysFreeString(updated_bstr);
    if (success) {
        const int caret_offset = static_cast<int>(first + replacement.size());
        if (!set_caret_after_offset(focused, caret_offset)) {
            // The text is already replaced; caret restoration is best-effort.
            // Keep the write as successful so we do not fall back and duplicate it.
        }
    }
    cleanup();
    return success;
}

} // namespace modore::windows
