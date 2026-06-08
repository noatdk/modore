#pragma once

#include "log.hpp"

#include <optional>
#include <string>
#include <vector>

namespace modore::windows {

struct ConversionResult {
    std::wstring committed;
    std::vector<std::wstring> candidates;
};

bool bootstrap_ime(Logger& logger);
bool warmup_ime(Logger& logger);
std::optional<std::wstring> convert_with_ime(const std::wstring& text, bool katakana, Logger& logger);
std::optional<ConversionResult> convert_with_ime_candidates(
    const std::wstring& text, bool katakana, Logger& logger);

} // namespace modore::windows
