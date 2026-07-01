// host_util.cpp — small string/utf8/json/env/file + logging-preview helpers.

#include "host_internal.hpp"

namespace modore_host {

std::string utf8_preview(const std::string &text, size_t max_chars) {
  if (text.empty()) {
    return "(empty)";
  }
  if (!g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()),
                       nullptr)) {
    std::string hex;
    constexpr size_t kMaxBytes = 24;
    for (size_t i = 0; i < text.size() && i < kMaxBytes; ++i) {
      if (!hex.empty()) {
        hex.push_back(' ');
      }
      char buf[4];
      std::snprintf(buf, sizeof(buf), "%02x",
                    static_cast<unsigned char>(text[i]));
      hex += buf;
    }
    if (text.size() > kMaxBytes) {
      hex += " ...";
    }
    return std::string("<invalid-utf8 bytes=") + std::to_string(text.size()) +
           " hex=" + hex + ">";
  }

  const char *begin = text.c_str();
  const char *cur = begin;
  size_t chars = 0;
  while (*cur && chars < max_chars) {
    cur = g_utf8_next_char(cur);
    ++chars;
  }
  std::string preview(begin, static_cast<size_t>(cur - begin));
  std::string out = preview;
  if (*cur) {
    out += "...";
  }
  return out;
}

void log_text_preview(const char *label, const std::string &text) {
  modore_log(g_log_scope_tag, "%s bytes=%zu utf8=\"%s\"",
             label ? label : "text", text.size(), utf8_preview(text).c_str());
}

// Synthetic keys are dispatched asynchronously in the focused client. `hyprctl
// dispatch` is synchronous for *our* process (waitpid), not for the app.
// Multi-ms sleeps are usually wasted on fast machines. Prefer yields plus a
// short optional nap (0–12ms) where we cannot observe readiness (select-all
// expansion, paste delivery, clipboard daemon).
void yield_to_compose_pipeline() {
  std::this_thread::yield();
  std::this_thread::yield();
}

// Experiment: 1 = all nap_after_compose_event() are no-ops (breaks Chromium
// paste-after-Ctrl+A).
#define MODORE_ZERO_NAP_EXPERIMENT 0

#if MODORE_ZERO_NAP_EXPERIMENT
void nap_after_compose_event(std::chrono::milliseconds /*unused*/) {}
#else
void nap_after_compose_event(std::chrono::milliseconds d) {
  yield_to_compose_pipeline();
  if (d.count() > 0) {
    std::this_thread::sleep_for(d);
  }
}
#endif

std::string getenv_string(const char *k, const char *def) {
  const char *s = std::getenv(k);
  return s ? std::string(s) : std::string(def);
}

std::string default_profile_dir() {
  std::string base = getenv_string("XDG_STATE_HOME", "");
  if (base.empty()) {
    base = getenv_string("HOME", "/tmp") + "/.local/state";
  }
  return base + "/modore";
}

// --- Clipboard helpers (xclip on X11, wl-paste on Wayland) -------------

bool command_ok(const char *cmd) { return std::system(cmd) == 0; }

// systemd --user and minimal environments often omit /usr/bin from PATH.
void augment_path_for_subprocesses() {
  const char *cur = std::getenv("PATH");
  const char *prefix = "/usr/local/sbin:/usr/local/bin:/usr/bin:/bin";
  if (!cur || !*cur) {
    ::setenv("PATH", prefix, 1);
    return;
  }
  std::string merged = std::string(prefix) + ":" + cur;
  ::setenv("PATH", merged.c_str(), 1);
}

std::string lower_ascii_copy(std::string s) {
  for (char &c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

// --- UTF-8 word boundaries (glib offsets = Unicode character indices) ---

void word_range_chars(const gchar *text, glong caret_chars, glong n_chars,
                      glong *start, glong *end) {
  glong len = n_chars;
  glong c = std::clamp<glong>(caret_chars, 0, len);
  auto is_ws = [](gunichar ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
  };
  glong st = c;
  while (st > 0) {
    const gchar *p = g_utf8_offset_to_pointer(text, st - 1);
    gunichar ch = g_utf8_get_char(p);
    if (is_ws(ch)) {
      break;
    }
    st--;
  }
  glong en = c;
  while (en < len) {
    const gchar *p = g_utf8_offset_to_pointer(text, en);
    gunichar ch = g_utf8_get_char(p);
    if (is_ws(ch)) {
      break;
    }
    en++;
  }
  if (st == en) {
    if (c < len) {
      *start = c;
      *end = c + 1;
      return;
    }
    if (c > 0) {
      *start = c - 1;
      *end = c;
      return;
    }
  }
  *start = st;
  *end = en;
}

void utf8_substr_bytes(const gchar *text, glong start_c, glong end_c,
                       std::string *out) {
  const gchar *a = g_utf8_offset_to_pointer(text, start_c);
  const gchar *b = g_utf8_offset_to_pointer(text, end_c);
  out->assign(a, b - a);
}

} // namespace modore_host
