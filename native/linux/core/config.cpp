#include "config.hpp"

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::string to_lower(std::string s) {
  for (char &c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

bool parse_key_value(std::string_view line, std::string *key_out,
                     std::string *val_out) {
  const auto eq = line.find('=');
  if (eq == std::string::npos) {
    return false;
  }
  *key_out = trim(std::string(line.substr(0, eq)));
  *val_out = trim(std::string(line.substr(eq + 1)));
  return !key_out->empty();
}

std::string default_config_dir() {
  const char *xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) {
    return std::string(xdg) + "/modore";
  }
  const char *home = std::getenv("HOME");
  if (home && *home) {
    return std::string(home) + "/.config/modore";
  }
  return std::string("/.config/modore");
}

std::string config_path() { return default_config_dir() + "/modore.conf"; }

KeySym resolve_keysym_token(const std::string &key_token) {
  const std::string k = to_lower(trim(key_token));

  if (k.size() == 1) {
    const char c = k[0];
    if (c >= 'a' && c <= 'z') {
      return static_cast<KeySym>(XK_a + (c - 'a'));
    }
    if (c >= 'A' && c <= 'Z') {
      return static_cast<KeySym>(XK_a + (c - 'A'));
    }
    if (c >= '0' && c <= '9') {
      return static_cast<KeySym>(XK_0 + (c - '0'));
    }
    if (c == '`') {
      return XK_grave;
    }
  }

  if (k.rfind("f", 0) == 0 && k.size() >= 2) {
    const int n = std::atoi(k.c_str() + 1);
    if (n >= 1 && n <= 24) {
      return static_cast<KeySym>(XK_F1 + (n - 1));
    }
  }

  static const struct {
    const char *name;
    KeySym sym;
  } k_map[] = {
      {"slash", XK_slash},
      {"period", XK_period},
      {"comma", XK_comma},
      {"semicolon", XK_semicolon},
      {"apostrophe", XK_apostrophe},
      {"quote", XK_apostrophe},
      {"grave", XK_grave},
      {"backquote", XK_grave},
      {"minus", XK_minus},
      {"equal", XK_equal},
      {"plus", XK_plus},
      {"space", XK_space},
      {"return", XK_Return},
      {"enter", XK_Return},
      {"tab", XK_Tab},
      {"escape", XK_Escape},
      {"esc", XK_Escape},
      {"backspace", XK_BackSpace},
      {"delete", XK_Delete},
      {"home", XK_Home},
      {"end", XK_End},
      {"page_up", XK_Page_Up},
      {"page_down", XK_Page_Down},
      {"left", XK_Left},
      {"right", XK_Right},
      {"up", XK_Up},
      {"down", XK_Down},
      {"bracketleft", XK_bracketleft},
      {"bracketright", XK_bracketright},
      {"backslash", XK_backslash},
  };
  for (const auto &e : k_map) {
    if (k == e.name) {
      return e.sym;
    }
  }

  KeySym ks = XStringToKeysym(key_token.c_str());
  if (ks != NoSymbol) {
    return ks;
  }
  return XStringToKeysym(k.c_str());
}

bool parse_hotkey_chord(const std::string &chord, X11HotkeySpec *out,
                        std::string *err) {
  if (chord.empty()) {
    *err = "empty hotkey";
    return false;
  }

  std::stringstream ss(chord);
  std::string segment;
  std::vector<std::string> parts;
  while (std::getline(ss, segment, '+')) {
    const std::string t = trim(segment);
    if (!t.empty()) {
      parts.push_back(t);
    }
  }
  if (parts.size() < 2) {
    *err = "hotkey must include at least one modifier and a key (e.g. "
           "Ctrl+Semicolon)";
    return false;
  }

  unsigned int mask = 0;
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    const std::string m = to_lower(parts[i]);
    if (m == "ctrl" || m == "control") {
      mask |= ControlMask;
    } else if (m == "shift") {
      mask |= ShiftMask;
    } else if (m == "alt" || m == "option" || m == "meta") {
      mask |= Mod1Mask;
    } else if (m == "super" || m == "win" || m == "command" || m == "cmd") {
      mask |= Mod4Mask;
    } else {
      *err = "unknown modifier: " + parts[i];
      return false;
    }
  }

  const KeySym ks = resolve_keysym_token(parts.back());
  if (ks == NoSymbol) {
    *err = "unknown key: " + parts.back();
    return false;
  }

  out->modifier_mask = mask;
  out->keysym = static_cast<std::uint64_t>(ks);
  return true;
}

void apply_default_hotkey(X11HotkeySpec *h) {
  h->modifier_mask = ControlMask;
  h->keysym = static_cast<std::uint64_t>(XK_semicolon);
}

void apply_conversion_defaults(ModoreConfig *out) {
  apply_default_hotkey(&out->conversion_hotkey);
  out->conversion_hotkey_description = "Ctrl+Semicolon (default)";
}

bool parse_non_negative_int(const std::string &value, int *out) {
  if (!out) {
    return false;
  }
  if (value.empty()) {
    return false;
  }
  char *end = nullptr;
  errno = 0;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (errno != 0 || !end || *end != '\0' || parsed < 0 ||
      parsed > static_cast<long>(std::numeric_limits<int>::max())) {
    return false;
  }
  *out = static_cast<int>(parsed);
  return true;
}

} // namespace

std::string modore_config_path() { return config_path(); }

bool load_modore_config(ModoreConfig *out, std::string *error_message) {
  error_message->clear();
  apply_conversion_defaults(out);

  const std::string path = modore_config_path();
  std::ifstream f(path);
  if (!f) {
    return true;
  }

  std::string current_section;
  std::string line;
  std::string hotkey_value;
  std::string bridge_backend_value;
  std::string pre_paste_delay_value;
  std::string paste_visibility_wait_value;
  std::string paste_visibility_step_value;
  std::string short_restore_delay_value;
  std::string long_restore_delay_value;
  std::string cycle_settle_delay_value;
  std::string cycle_post_inject_delay_value;
  std::string cycle_backspace_step_value;
  std::string pickup_start_delay_value;
  std::string atspi_direct_settle_delay_value;
  std::string atspi_replacement_settle_delay_value;
  std::string clear_poll_max_wait_value;
  std::string clear_poll_step_value;
  std::string wayland_select_settle_value;
  std::string wayland_copy_poll_value;
  std::string wayland_copy_poll_step_value;
  while (std::getline(f, line)) {
    const auto hash = line.find('#');
    if (hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    line = trim(line);
    if (line.empty()) {
      continue;
    }
    if (!line.empty() && line.front() == '[' && line.back() == ']') {
      current_section = to_lower(trim(line.substr(1, line.size() - 2)));
      continue;
    }
    std::string k;
    std::string v;
    if (!parse_key_value(line, &k, &v)) {
      continue;
    }
    const std::string k_l = to_lower(k);
    if (current_section == "conversion" && k_l == "hotkey") {
      hotkey_value = v;
    } else if (current_section == "bridge" && k_l == "mozc_backend") {
      bridge_backend_value = v;
    } else if (current_section == "clipboard") {
      if (k_l == "pre_copy_delay_ms" || k_l == "pre_paste_delay_ms") {
        pre_paste_delay_value = v;
      } else if (k_l == "read_timeout_ms" ||
                 k_l == "paste_visibility_wait_ms") {
        paste_visibility_wait_value = v;
      } else if (k_l == "read_step_ms" || k_l == "paste_visibility_step_ms") {
        paste_visibility_step_value = v;
      } else if (k_l == "restore_clipboard_delay_ms" ||
                 k_l == "short_restore_delay_ms") {
        short_restore_delay_value = v;
      } else if (k_l == "restore_clipboard_long_delay_ms" ||
                 k_l == "pickup_restore_delay_ms") {
        long_restore_delay_value = v;
      } else if (k_l == "cycle_settle_delay_ms") {
        cycle_settle_delay_value = v;
      } else if (k_l == "cycle_post_inject_delay_ms") {
        cycle_post_inject_delay_value = v;
      } else if (k_l == "cycle_backspace_step_ms") {
        cycle_backspace_step_value = v;
      } else if (k_l == "pickup_start_delay_ms") {
        pickup_start_delay_value = v;
      } else if (k_l == "atspi_direct_settle_delay_ms") {
        atspi_direct_settle_delay_value = v;
      } else if (k_l == "atspi_replacement_settle_delay_ms") {
        atspi_replacement_settle_delay_value = v;
      } else if (k_l == "clear_poll_max_wait_ms") {
        clear_poll_max_wait_value = v;
      } else if (k_l == "clear_poll_step_ms") {
        clear_poll_step_value = v;
      } else if (k_l == "wayland_select_settle_ms") {
        wayland_select_settle_value = v;
      } else if (k_l == "wayland_copy_poll_ms") {
        wayland_copy_poll_value = v;
      } else if (k_l == "wayland_copy_poll_step_ms") {
        wayland_copy_poll_step_value = v;
      }
    }
  }

  if (!hotkey_value.empty()) {
    X11HotkeySpec parsed{};
    std::string err;
    if (!parse_hotkey_chord(hotkey_value, &parsed, &err)) {
      *error_message = err;
      apply_conversion_defaults(out);
      return false;
    }
    out->conversion_hotkey = parsed;
    out->conversion_hotkey_description = trim(hotkey_value);
  }

  // Normalize [bridge] mozc_backend to the bridge's MODORE_MOZC_BACKEND token.
  // Unrecognized / absent leaves it empty so the host keeps the bridge default
  // (built-in Mozc) rather than failing init on a typo.
  if (!bridge_backend_value.empty()) {
    const std::string b = to_lower(bridge_backend_value);
    if (b == "atzc") {
      out->mozc_backend = "atzc";
    } else if (b == "built-in" || b == "built_in" || b == "builtin" ||
               b == "oss") {
      out->mozc_backend = "oss";
    }
  }

  int parsed_int = 0;
  if (parse_non_negative_int(pre_paste_delay_value, &parsed_int)) {
    out->clipboard_pre_paste_delay_ms = parsed_int;
  }
  if (parse_non_negative_int(paste_visibility_wait_value, &parsed_int)) {
    out->clipboard_paste_visibility_wait_ms = parsed_int;
  }
  if (parse_non_negative_int(paste_visibility_step_value, &parsed_int)) {
    out->clipboard_paste_visibility_step_ms = parsed_int;
  }
  if (parse_non_negative_int(short_restore_delay_value, &parsed_int)) {
    out->clipboard_short_restore_delay_ms = parsed_int;
  }
  if (parse_non_negative_int(long_restore_delay_value, &parsed_int)) {
    out->clipboard_long_restore_delay_ms = parsed_int;
  }
  if (parse_non_negative_int(cycle_settle_delay_value, &parsed_int)) {
    out->clipboard_cycle_settle_delay_ms = parsed_int;
  }
  if (parse_non_negative_int(cycle_post_inject_delay_value, &parsed_int)) {
    out->clipboard_cycle_post_inject_delay_ms = parsed_int;
  }
  if (parse_non_negative_int(cycle_backspace_step_value, &parsed_int)) {
    out->clipboard_cycle_backspace_step_ms = parsed_int;
  }
  if (parse_non_negative_int(pickup_start_delay_value, &parsed_int)) {
    out->clipboard_pickup_start_delay_ms = parsed_int;
  }
  if (parse_non_negative_int(atspi_direct_settle_delay_value, &parsed_int)) {
    out->clipboard_atspi_direct_settle_delay_ms = parsed_int;
  }
  if (parse_non_negative_int(atspi_replacement_settle_delay_value,
                             &parsed_int)) {
    out->clipboard_atspi_replacement_settle_delay_ms = parsed_int;
  }
  if (parse_non_negative_int(clear_poll_max_wait_value, &parsed_int)) {
    out->clipboard_clear_poll_max_wait_ms = parsed_int;
  }
  if (parse_non_negative_int(clear_poll_step_value, &parsed_int)) {
    out->clipboard_clear_poll_step_ms = parsed_int;
  }
  if (parse_non_negative_int(wayland_select_settle_value, &parsed_int)) {
    out->clipboard_wayland_select_settle_ms = parsed_int;
  }
  if (parse_non_negative_int(wayland_copy_poll_value, &parsed_int)) {
    out->clipboard_wayland_copy_poll_ms = parsed_int;
  }
  if (parse_non_negative_int(wayland_copy_poll_step_value, &parsed_int)) {
    out->clipboard_wayland_copy_poll_step_ms = parsed_int;
  }
  return true;
}
