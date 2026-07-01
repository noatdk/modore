#pragma once

#include "config.hpp"

#include <functional>
#include <string>

bool start_evdev_hotkey_monitor(const X11HotkeySpec &hotkey,
                                const std::string &description,
                                std::function<void()> on_trigger,
                                std::function<void()> on_non_hotkey_keydown,
                                std::string *error_message);

// Best-effort snapshot of the current modifier state seen by the evdev hotkey
// monitor. Bits mirror the X11 modifier mask bits used by X11HotkeySpec.
unsigned int evdev_current_modifier_mask();
