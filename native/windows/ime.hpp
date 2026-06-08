#pragma once

#include "log.hpp"

#include <optional>
#include <string>

namespace modore::windows {

bool bootstrap_ime(Logger& logger);
std::optional<std::wstring> convert_with_ime(const std::wstring& text, bool katakana, Logger& logger);

} // namespace modore::windows
