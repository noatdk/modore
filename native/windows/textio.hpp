#pragma once

#include <optional>
#include <string>

namespace modore::windows {

std::optional<std::wstring> focused_selection_text();
std::optional<std::wstring> focused_editable_text();
bool replace_focused_selection_text(const std::wstring& selected_text, const std::wstring& replacement);

} // namespace modore::windows
