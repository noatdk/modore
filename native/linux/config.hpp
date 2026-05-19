// modore.conf — minimal INI loader (XDG path ~/.config/modore/modore.conf).
//
// Format (extend with new sections/keys as needed):
//
//   # comment
//   [conversion]
//   hotkey = Ctrl+Semicolon
//
// Modifiers (combine with +): Ctrl, Shift, Alt, Super.
// Key names: Slash, Period, Comma, a–z, 0–9, Space, Return, Tab, Escape,
// F1–F24, Minus, Equal, Grave, Backslash, BracketLeft, BracketRight, Semicolon,
// Quote, or any keysym string accepted by XStringToKeysym(3) (e.g. "slash").

#pragma once

#include <X11/X.h>

#include <cstdint>
#include <string>

struct X11HotkeySpec {
  // Bitmask of ShiftMask | ControlMask | Mod1Mask | Mod4Mask | Mod5Mask.
  unsigned int modifier_mask{Mod4Mask};
  // Logical keysym (e.g. XK_semicolon); keycodes are resolved per Display.
  std::uint64_t keysym{0x003b};  // XK_semicolon
};

struct ModoreConfig {
  X11HotkeySpec conversion_hotkey{};
  // Human-readable chord for logs (defaults to "Ctrl+Semicolon (default)").
  std::string conversion_hotkey_description;
};

// Populates defaults when the file is missing or keys are absent.
// Returns false only on a present-but-invalid hotkey (see *error_message).
[[nodiscard]] bool load_modore_config(ModoreConfig* out, std::string* error_message);
