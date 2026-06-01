#pragma once

#include "config.hpp"
#include "log.hpp"

#include <functional>
#include <optional>
#include <string>

namespace modore::windows {

using PickupConverter = std::function<std::optional<std::wstring>(const std::wstring&)>;

bool perform_pickup(const ConfigSnapshot& config, Logger& logger, const PickupConverter& convert);

} // namespace modore::windows
