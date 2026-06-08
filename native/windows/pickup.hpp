#pragma once

#include "config.hpp"
#include "log.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace modore::windows {

struct PickupConversionResult {
    std::wstring committed;
    std::vector<std::wstring> candidates;
};

using PickupConverter = std::function<std::optional<PickupConversionResult>(const std::wstring&)>;

bool perform_pickup(const ConfigSnapshot& config, Logger& logger, const PickupConverter& convert);
bool cycle_last_pickup(Logger& logger);

} // namespace modore::windows
