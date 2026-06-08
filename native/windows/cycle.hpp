#pragma once

#include "log.hpp"
#include "pickup.hpp"

namespace modore::windows {

void reset_cycle();
bool has_cycle_session();
void remember_cycle(
    const std::wstring& field_text,
    const std::wstring& original_text,
    const PickupConversionResult& conversion);
bool cycle_last_pickup(Logger& logger);
bool cycle_previous_pickup(Logger& logger);
bool undo_last_pickup(Logger& logger);

} // namespace modore::windows
