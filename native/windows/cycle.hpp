#pragma once

#include "log.hpp"
#include "pickup.hpp"

namespace modore::windows {

void reset_cycle();
void remember_cycle(
    const std::wstring& field_text,
    const PickupConversionResult& conversion);
bool cycle_last_pickup(Logger& logger);
bool cycle_previous_pickup(Logger& logger);

} // namespace modore::windows
