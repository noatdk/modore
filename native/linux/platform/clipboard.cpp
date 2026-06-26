// clipboard.cpp — clipboard read/write/poll (wl-clipboard + xclip).

#include "host_internal.hpp"

namespace modore_host {

bool read_clipboard_cmd(const char *cmd, std::string *out) {
  out->clear();
  FILE *f = popen(cmd, "r");
  if (!f) {
    return false;
  }
  char buf[4096];
  while (size_t n = fread(buf, 1, sizeof(buf), f)) {
    out->append(buf, n);
  }
  int st = pclose(f);
  return WIFEXITED(st) && WEXITSTATUS(st) == 0 && !out->empty();
}

const char *resolve_wl_paste() {
  static const char *cached = nullptr;
  if (cached) {
    return cached;
  }
  static const char *kCandidates[] = {"/usr/bin/wl-paste",
                                      "/usr/local/bin/wl-paste"};
  for (auto *p : kCandidates) {
    if (::access(p, X_OK) == 0) {
      cached = p;
      return cached;
    }
  }
  augment_path_for_subprocesses();
  if (command_ok("command -v wl-paste >/dev/null 2>&1")) {
    cached = "wl-paste";
    return cached;
  }
  cached = "wl-paste"; // best effort for error messages
  return cached;
}

const char *resolve_wl_copy() {
  static const char *cached = nullptr;
  if (cached) {
    return cached;
  }
  static const char *kCandidates[] = {"/usr/bin/wl-copy",
                                      "/usr/local/bin/wl-copy"};
  for (auto *p : kCandidates) {
    if (::access(p, X_OK) == 0) {
      cached = p;
      return cached;
    }
  }
  augment_path_for_subprocesses();
  if (command_ok("command -v wl-copy >/dev/null 2>&1")) {
    cached = "wl-copy";
    return cached;
  }
  cached = "wl-copy";
  return cached;
}

bool wl_clipboard_available() {
  if (!std::getenv("WAYLAND_DISPLAY")) {
    return false;
  }
  const char *p = resolve_wl_paste();
  if (p && p[0] == '/' && ::access(p, X_OK) == 0) {
    return true;
  }
  augment_path_for_subprocesses();
  return command_ok("command -v wl-paste >/dev/null 2>&1");
}

void trim_trailing_crlf_inplace(std::string *s) {
  if (!s) {
    return;
  }
  while (!s->empty() && (s->back() == '\n' || s->back() == '\r')) {
    s->pop_back();
  }
}

bool clipboard_normalized_equal(const std::string &a, const std::string &b) {
  std::string x = a;
  std::string y = b;
  trim_trailing_crlf_inplace(&x);
  trim_trailing_crlf_inplace(&y);
  return x == y;
}

// Pick the most preferred text MIME wl-clipboard advertises right now. Returns
// "" when the current offer is binary-only (image/png from a screenshot,
// application/octet-stream, etc). Avoids the historical foot-gun where
// `wl-paste 2>/dev/null` dumps raw PNG bytes into a std::string and downstream
// sees "\x89PNG\r\n\x1a\n" as garbage in the focused field.
std::string wl_pick_text_mime(const char *primary_flag) {
  if (!wl_clipboard_available()) {
    return std::string();
  }
  const std::string base = resolve_wl_paste();
  std::string cmd =
      base + (primary_flag ? std::string(" ") + primary_flag : std::string()) +
      " -l 2>/dev/null";
  std::string list;
  if (!read_clipboard_cmd(cmd.c_str(), &list) || list.empty()) {
    return std::string();
  }
  static const char *kPreferred[] = {
      "UTF8_STRING",
      "text/plain;charset=utf-8",
      "text/plain;charset=UTF-8",
      "text/plain",
      "STRING",
      "TEXT",
  };
  for (const char *want : kPreferred) {
    std::string needle = std::string("\n") + want + "\n";
    std::string hay = std::string("\n") + list + "\n";
    if (hay.find(needle) != std::string::npos) {
      return std::string(want);
    }
  }
  return std::string();
}

// Drop the read entirely when wl-paste hands us bytes that aren't valid UTF-8 —
// text fields can't display arbitrary binary anyway, and feeding it to Mozc /
// typing it back is the bug the user saw as "he❱PNG" (PNG file header leaking
// through a generic wl-paste call).
bool read_wl_offer_text_only(const char *primary_flag, std::string *out) {
  out->clear();
  if (!wl_clipboard_available()) {
    return false;
  }
  const std::string mime = wl_pick_text_mime(primary_flag);
  if (mime.empty()) {
    return false;
  }
  const std::string base = resolve_wl_paste();
  std::string cmd =
      base + (primary_flag ? std::string(" ") + primary_flag : std::string()) +
      " -t " + mime + " 2>/dev/null";
  std::string raw;
  if (!read_clipboard_cmd(cmd.c_str(), &raw)) {
    return false;
  }
  if (!raw.empty() &&
      !g_utf8_validate(raw.c_str(), static_cast<gssize>(raw.size()), nullptr)) {
    logf("clipboard: dropping %zu-byte wl-paste read — not valid UTF-8 (binary "
         "leaked via %s)",
         raw.size(), mime.c_str());
    return false;
  }
  out->swap(raw);
  return true;
}

// wl-paste without --primary: clipboard selection only (not primary buffer).
bool read_wl_clip_offer(std::string *out) {
  return read_wl_offer_text_only(nullptr, out);
}

// wl-paste --primary only (Wayland middle-click buffer; often synced from
// highlight).
bool read_wl_primary_offer(std::string *out) {
  return read_wl_offer_text_only("--primary", out);
}

bool read_clipboard(std::string *out) {
  if (wl_clipboard_available()) {
    if (read_wl_offer_text_only(nullptr, out)) {
      return true;
    }
    return read_wl_offer_text_only("--primary", out);
  }
  if (read_clipboard_cmd("xclip -selection clipboard -o 2>/dev/null", out)) {
    if (!out->empty() &&
        !g_utf8_validate(out->c_str(), static_cast<gssize>(out->size()),
                         nullptr)) {
      logf("clipboard: dropping %zu-byte xclip CLIPBOARD read — not valid "
           "UTF-8 (binary?)",
           out->size());
      out->clear();
    } else {
      return true;
    }
  }
  if (read_clipboard_cmd("xclip -selection primary -o 2>/dev/null", out)) {
    if (!out->empty() &&
        !g_utf8_validate(out->c_str(), static_cast<gssize>(out->size()),
                         nullptr)) {
      logf("clipboard: dropping %zu-byte xclip PRIMARY read — not valid UTF-8 "
           "(binary?)",
           out->size());
      out->clear();
      return false;
    }
    return true;
  }
  return false;
}

bool write_clipboard(const std::string &s) {
  if (wl_clipboard_available()) {
    const char *variants[] = {
        " --type text/plain;charset=utf-8 2>/dev/null",
        " --type text/plain 2>/dev/null",
        " 2>/dev/null",
    };
    for (const char *suf : variants) {
      std::string cmd = std::string(resolve_wl_copy()) + suf;
      FILE *f = popen(cmd.c_str(), "w");
      if (!f) {
        continue;
      }
      fwrite(s.data(), 1, s.size(), f);
      (void)std::fflush(f);
      int st = pclose(f);
      if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        return true;
      }
    }
    return false;
  }
  FILE *f = popen("xclip -selection clipboard", "w");
  if (!f) {
    return false;
  }
  fwrite(s.data(), 1, s.size(), f);
  int st = pclose(f);
  return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

bool wl_clipboard_trimmed_empty(const std::string &c) {
  std::string x = c;
  trim_trailing_crlf_inplace(&x);
  trim_in_place_ascii(&x);
  return x.empty();
}

// After wl-copy "", the daemon often updates in <20ms; poll instead of a long
// blind sleep.
bool poll_wl_clipboard_cleared(int max_wait_ms, int step_ms) {
  if (!wl_clipboard_available() || max_wait_ms <= 0) {
    return true;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    std::string c;
    read_wl_clip_offer(&c);
    if (wl_clipboard_trimmed_empty(c)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
  }
  return false;
}

// Wait until wl-paste matches `expected` (e.g. fresh payload before synthetic
// Ctrl+V).
bool wait_wl_clipboard_equals_normalized(const std::string &expected,
                                         int max_wait_ms, int step_ms) {
  if (!wl_clipboard_available() || max_wait_ms <= 0) {
    return false;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
  if (wl_clipboard_trimmed_empty(expected)) {
    while (std::chrono::steady_clock::now() < deadline) {
      std::string c;
      read_wl_clip_offer(&c);
      if (wl_clipboard_trimmed_empty(c)) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    }
    return false;
  }
  while (std::chrono::steady_clock::now() < deadline) {
    std::string c;
    read_wl_clip_offer(&c);
    if (clipboard_normalized_equal(c, expected)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
  }
  return false;
}

} // namespace modore_host
