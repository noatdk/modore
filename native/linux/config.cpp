#include "config.hpp"

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
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
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

bool parse_key_value(std::string_view line, std::string* key_out, std::string* val_out) {
  const auto eq = line.find('=');
  if (eq == std::string::npos) {
    return false;
  }
  *key_out = trim(std::string(line.substr(0, eq)));
  *val_out = trim(std::string(line.substr(eq + 1)));
  return !key_out->empty();
}

std::string default_config_dir() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) {
    return std::string(xdg) + "/modore";
  }
  const char* home = std::getenv("HOME");
  if (home && *home) {
    return std::string(home) + "/.config/modore";
  }
  return std::string("/.config/modore");
}

KeySym resolve_keysym_token(const std::string& key_token) {
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
    const char* name;
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
  for (const auto& e : k_map) {
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

bool parse_hotkey_chord(const std::string& chord, X11HotkeySpec* out, std::string* err) {
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
    *err = "hotkey must include at least one modifier and a key (e.g. Ctrl+Semicolon)";
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

void apply_default_hotkey(X11HotkeySpec* h) {
  h->modifier_mask = ControlMask;
  h->keysym = static_cast<std::uint64_t>(XK_semicolon);
}

void apply_conversion_defaults(ModoreConfig* out) {
  apply_default_hotkey(&out->conversion_hotkey);
  out->conversion_hotkey_description = "Ctrl+Semicolon (default)";
}

}  // namespace

bool load_modore_config(ModoreConfig* out, std::string* error_message) {
  error_message->clear();
  apply_conversion_defaults(out);

  const std::string path = default_config_dir() + "/modore.conf";
  std::ifstream f(path);
  if (!f) {
    return true;
  }

  std::string current_section;
  std::string line;
  std::string hotkey_value;
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
    }
  }

  if (hotkey_value.empty()) {
    return true;
  }

  X11HotkeySpec parsed{};
  std::string err;
  if (!parse_hotkey_chord(hotkey_value, &parsed, &err)) {
    *error_message = err;
    apply_conversion_defaults(out);
    return false;
  }
  out->conversion_hotkey = parsed;
  out->conversion_hotkey_description = trim(hotkey_value);
  return true;
}
