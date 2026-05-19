#pragma once

#include "config.hpp"

#include <functional>
#include <string>

bool start_evdev_hotkey_monitor(const X11HotkeySpec& hotkey, const std::string& description,
                                std::function<void()> on_trigger, std::string* error_message);
