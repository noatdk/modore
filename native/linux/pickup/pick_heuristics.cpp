// pick_heuristics.cpp — romaji / mojibake / narrowing heuristics for picks.

#include "host_internal.hpp"

namespace modore_host {

// Heuristic matching macOS: line-copy detection ---------------------------

bool looks_like_line_copy(const std::string &s) {
  if (s.find('\n') != std::string::npos || s.find('\r') != std::string::npos) {
    return true;
  }
  // Long single-line copies are usually the real selection from browser/UI
  // widgets. The old threshold (>200 bytes) matched normal Romaji picks and
  // wrongly forced Ctrl+Shift+Left ("stopping at selection") when the trimmed
  // first-line rule failed too.
  return s.size() > 524288;
}

void trim_in_place_ascii(std::string *s) {
  if (!s) {
    return;
  }
  while (!s->empty() && std::isspace(static_cast<unsigned char>(s->front()))) {
    s->erase(s->begin());
  }
  while (!s->empty() && std::isspace(static_cast<unsigned char>(s->back()))) {
    s->pop_back();
  }
}

// Nautilus path bar exposes PRIMARY as a full POSIX path (~34 chars) while the
// user only sees the last segment highlighted (e.g. "henkan"). Convert the tail
// so Mozc gets the visible token. Returns true iff we rewrote picked to that
// final segment — then glyph-count erase is unsafe.
bool maybe_narrow_path_primary_pick(std::string *picked) {
  if (!picked || picked->empty()) {
    return false;
  }
  if (picked->find('/') == std::string::npos) {
    return false;
  }
  const size_t last_slash = picked->find_last_of('/');
  if (last_slash == std::string::npos || last_slash + 1 >= picked->size()) {
    return false;
  }
  std::string tail = picked->substr(last_slash + 1);
  trim_trailing_crlf_inplace(&tail);
  trim_in_place_ascii(&tail);
  if (tail.empty() || tail.find_first_of("\n\r") != std::string::npos) {
    return false;
  }
  constexpr size_t kMaxTail = 512;
  if (tail.size() <= kMaxTail && tail.size() < picked->size()) {
    logf("clipboard: narrowed path-like PRIMARY (%zu bytes) → final segment "
         "(%zu bytes)",
         picked->size(), tail.size());
    picked->swap(tail);
    return true;
  }
  return false;
}

// Mozc will “convert” arbitrary Latin (paths, flags) into phonetic kana/kanji +
// leftover ASCII, which shows up as garbage in Walker (e.g.
// ~/.local/bin/modore-host --trigger → okashi·bin·…).
bool clipboard_pick_probably_not_romaji_field(const std::string &s) {
  if (s.find("modore-host") != std::string::npos) {
    return true;
  }
  if (s.find("#!/") != std::string::npos) {
    return true;
  }
  const bool has_pathish =
      s.find('/') != std::string::npos || s.find('~') != std::string::npos;
  const bool has_double_dash = s.find("--") != std::string::npos;
  if (has_pathish && has_double_dash) {
    return true;
  }
  if (has_pathish && (s.find("bin/") != std::string::npos ||
                      s.find(".local/") != std::string::npos)) {
    return true;
  }
  return false;
}

// Cursor / VS Code / Electron / CLIs often mirror placeholder or shortcut hint
// text into PRIMARY
// ("Add a follow-up", "ctrl+c to stop") — Mozc turns that into mojibake soup.
// PRIMARY is also
// **global**: a focused terminal or TUI can publish this while the user thinks
// another app (browser) "owns" the edit field — modore must not trust that
// PRIMARY as the pick.
const char *
clipboard_first_matching_modifier_or_ui_hint_needle_ci(const std::string &s) {
  if (s.size() < 6) {
    return nullptr;
  }
  std::string low;
  low.reserve(s.size());
  for (unsigned char c : s) {
    low.push_back(static_cast<char>(std::tolower(c)));
  }
  static const char *kNeedles[] = {
      "ctrl+",     "cmd+",      "meta+",     "shift+",       "alt+",
      "super+",    "follow-up", "follow up", "to stop",      "esc to",
      "press esc", "shortcut",  "keyboard",  "add a follow", "addafollow",
  };
  for (const char *n : kNeedles) {
    if (low.find(n) != std::string::npos) {
      return n;
    }
  }
  return nullptr;
}

// TUI progress bars (e.g. streaming CLIs) often land in PRIMARY as UTF-8 block
// glyphs (U+2580–U+259F).
bool wl_primary_dominated_by_block_elements(const std::string &s) {
  size_t n_block = 0;
  for (size_t i = 0; i + 3 <= s.size();) {
    if (static_cast<unsigned char>(s[i]) == 0xe2 &&
        static_cast<unsigned char>(s[i + 1]) == 0x96) {
      const unsigned char t = static_cast<unsigned char>(s[i + 2]);
      if (t >= 0x80 && t <= 0x9f) {
        ++n_block;
        i += 3;
        continue;
      }
    }
    ++i;
  }
  return n_block >= 8;
}

bool wl_primary_looks_like_stale_global_chrome(const std::string &s) {
  return clipboard_first_matching_modifier_or_ui_hint_needle_ci(s) != nullptr ||
         wl_primary_dominated_by_block_elements(s);
}

// PRIMARY fast-path only (empty CLIPBOARD + skip Ctrl+C): must not fire when
// GTK mirrors a post-conversion CJK slice — mozore often clears CLIPBOARD first
// so baseline_clip is empty, which incorrectly made PRIMARY the pick every
// time.
bool wl_primary_is_utf8_bounded_ascii_only_fast_pick(const std::string &s) {
  if (s.empty()) {
    return false;
  }
  for (unsigned char c : s) {
    if (c >= 0x80u) {
      return false;
    }
  }
  return true;
}

bool clipboard_pick_probably_ide_ui_hint(const std::string &s) {
  const char *n = clipboard_first_matching_modifier_or_ui_hint_needle_ci(s);
  if (!n) {
    return false;
  }
  logf("clipboard: pick embeds IDE/UI shortcut hint (matched '%s') — skipping "
       "Mozc",
       n);
  return true;
}

// Many inputs arrive as one logical line; truncate very large blobs (browser
// dumps).
bool clipboard_first_reasonable_line(const std::string &raw,
                                     std::string *picked) {
  picked->clear();
  size_t pos = 0;
  constexpr size_t kMaxFirstLinePick = 262144;
  while (pos < raw.size()) {
    while (pos < raw.size() && (raw[pos] == '\n' || raw[pos] == '\r')) {
      ++pos;
    }
    const size_t nl = raw.find_first_of("\n\r", pos);
    std::string first =
        nl == std::string::npos ? raw.substr(pos) : raw.substr(pos, nl - pos);
    trim_in_place_ascii(&first);
    if (!first.empty()) {
      if (first.size() > kMaxFirstLinePick) {
        first.resize(kMaxFirstLinePick);
      }
      picked->swap(first);
      return true;
    }
    if (nl == std::string::npos) {
      break;
    }
    pos = nl;
  }
  return false;
}

// GTK/Qt/WebKit mirror the highlighted range to PRIMARY. After Ctrl+C the
// clipboard often contains a whole line (or paragraph) while PRIMARY tracks the
// user's smaller selection.
bool wl_try_primary_as_highlighted_span(const std::string &baseline_primary,
                                        const std::string &clip_text,
                                        std::string *picked) {
  picked->clear();
  if (!wl_clipboard_available() || clip_text.empty()) {
    return false;
  }
  std::string prim;
  if (!read_wl_primary_offer(&prim)) {
    return false;
  }
  trim_trailing_crlf_inplace(&prim);
  trim_in_place_ascii(&prim);
  if (prim.empty() || clipboard_normalized_equal(prim, baseline_primary)) {
    return false;
  }
  if (wl_primary_looks_like_stale_global_chrome(prim)) {
    return false;
  }
  const size_t nl = clip_text.find_first_of("\n\r");
  std::string fl =
      nl == std::string::npos ? clip_text : clip_text.substr(0, nl);
  trim_trailing_crlf_inplace(&fl);
  trim_in_place_ascii(&fl);
  if (fl.empty() || fl.find(prim) == std::string::npos ||
      prim.size() > fl.size()) {
    return false;
  }
  if (prim.size() < fl.size()) {
    picked->assign(prim);
    return true;
  }
  return false;
}

// Hyprland/Omarchy universal paste is Shift+Insert; wtype often fails for CJK,
// and AT-SPI STRING is unreliable on Wayland.
bool utf8_contains_non_ascii(const std::string &utf8) {
  for (unsigned char c : utf8) {
    if (c >= 0x80) {
      return true;
    }
  }
  return false;
}

// Clipboard span that still looks like "henkan" / romaji ASCII only (Mozc roman
// input slice).
bool pick_is_plain_ascii_romaji(const std::string &pick) {
  if (pick.empty() || pick.size() > 1536) {
    return false;
  }
  for (unsigned char c : pick) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      continue;
    }
    if (c == '-' || c == '_' || c == '\'') {
      continue;
    }
    return false;
  }
  return true;
}

// Omnibox / IME often merges editable romaji with a pasted or suggested
// YouTube/query tail
// (`v=jZX...`, `&list=`). Sending the whole blob to Mozc destroys the URL ("じ"
// replacing "j"). Returns true iff *picked shrank to a leading ASCII-only
// romaji slice.
bool leading_ascii_romaji_token_prefix(const std::string &s,
                                       std::string *token) {
  token->clear();
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
    ++i;
  }
  const size_t start = i;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0x80) {
      break;
    }
    if (std::isalnum(c) || c == '-' || c == '_' || c == '\'') {
      ++i;
      continue;
    }
    break;
  }
  if (i <= start) {
    return false;
  }
  token->assign(s, start, i - start);
  return pick_is_plain_ascii_romaji(*token);
}

// Clipboard/omnibox picks sometimes include stray Unicode (e.g. U+2771 `❱`, RTL
// marks, or random multibyte glyphs from earlier IME state) wedged between
// romaji and an ASCII suffix like "PNG". Mozc converts the prefix but the
// suffix survives as literal ASCII (visible as "he❱PNG" stays "he❱PNG" or
// becomes "へPNG"). Trim to the leading romaji token; caller forces Ctrl+A
// clear so glyph BackSpace can't leave the trailing junk behind.
bool trim_pick_leading_romaji_if_utf8_contaminated(std::string *picked) {
  if (!picked || picked->empty()) {
    return false;
  }
  bool any_multibyte = false;
  for (unsigned char c : *picked) {
    if (c >= 0x80) {
      any_multibyte = true;
      break;
    }
  }
  if (!any_multibyte) {
    return false;
  }
  std::string token;
  if (!leading_ascii_romaji_token_prefix(*picked, &token) || token.empty() ||
      token.size() >= picked->size()) {
    return false;
  }
  logf("clipboard: trimmed UTF-8–contaminated pick before Mozc (%zu → %zu "
       "bytes)",
       picked->size(), token.size());
  picked->swap(token);
  return true;
}

size_t omniboz_earliest_url_like_marker_ci(std::string *low_ascii_out,
                                           const std::string &raw) {
  low_ascii_out->clear();
  low_ascii_out->reserve(raw.size());
  for (unsigned char c : raw) {
    low_ascii_out->push_back(static_cast<char>(std::tolower(c)));
  }
  const std::string &low = *low_ascii_out;
  size_t cut = std::string::npos;
  static const char *kMarkers[] = {
      "http://",  "https://", "www.youtube", "youtube.com", "youtu.be/",
      "youtube.", "&list=",   "&index=",     "&feature=",   "&v=",
      "?v=",      "/watch",   "watch?",
  };
  for (const char *m : kMarkers) {
    const size_t p = low.find(m);
    if (p != std::string::npos) {
      cut = std::min(cut, p);
    }
  }
  // `v=jZX...` query param — avoid matching "...inv=alice" (`v=` must follow ?
  // / & / / )
  for (size_t i = 0; i + 2 < low.size(); ++i) {
    if (low[i] == 'v' && low[i + 1] == '=' &&
        (i == 0 || low[i - 1] == '?' || low[i - 1] == '&' ||
         low[i - 1] == '/' || low[i - 1] == '#')) {
      cut = std::min(cut, i);
    }
  }
  return cut;
}

bool maybe_narrow_omnibox_url_contaminated_pick(std::string *picked) {
  if (!picked || picked->size() < 8) {
    return false;
  }
  std::string low;
  const size_t cut = omniboz_earliest_url_like_marker_ci(&low, *picked);
  if (cut == std::string::npos || cut == 0) {
    return false;
  }

  std::string prefix = picked->substr(0, cut);
  trim_trailing_crlf_inplace(&prefix);
  trim_in_place_ascii(&prefix);
  if (prefix.empty()) {
    return false;
  }

  std::string token;
  if (!leading_ascii_romaji_token_prefix(prefix, &token)) {
    return false;
  }
  constexpr size_t kMinTok = 2;
  constexpr size_t kMaxTok = 512;
  if (token.size() < kMinTok || token.size() > kMaxTok) {
    return false;
  }

  const bool shrunk = token.size() < picked->size();
  if (!shrunk) {
    return false;
  }

  logf("clipboard: omnibox/url tail detected — Mozc slice \"%s\" (%zu bytes) "
       "from %zu-byte pick "
       "(dropped YouTube/query junk)",
       token.c_str(), token.size(), picked->size());
  picked->swap(token);
  return true;
}

// Long picks with substantial real kana/CJK are unlikely to be Latin-on-UTF-8
// mojibake; blocking those yields false positives (notify + field nuking) while
// paste/sync is already handled elsewhere.
bool pick_looks_like_mojibake_garbage(const std::string &pick) {
  if (pick.empty() || pick.size() < 12) {
    return false;
  }
  if (!g_utf8_validate(pick.c_str(), static_cast<gssize>(pick.size()),
                       nullptr)) {
    return true;
  }
  const gchar *p = pick.c_str();
  glong latin_stutter = 0;
  glong jp_mass = 0;
  gunichar uch;
  auto jp_glyph = [](gunichar u) -> bool {
    return (u >= 0x3040 && u <= 0x309f) || (u >= 0x30a0 && u <= 0x30ff) ||
           (u >= 0x4e00 && u <= 0x9fff) || (u >= 0x3400 && u <= 0x4dbf);
  };
  while (*p) {
    uch = g_utf8_get_char(p);
    if (jp_glyph(uch)) {
      ++jp_mass;
    }
    if (uch == 0xfffd || uch == 0x25a1 || uch == 0x2592 || uch == 0x25af) {
      latin_stutter += 12;
    }
    if ((uch >= 0x0080 && uch <= 0x00bf) || uch == 0x00c2 || uch == 0x00c3 ||
        uch == 0x00c4 || uch == 0x00e2 || uch == 0x00e3 || uch == 0x00aa ||
        uch == 0x00ba || uch == 0x00ac || uch == 0x00a1 || uch == 0x00a3 ||
        uch == 0x00a9 || uch == 0x00ae || uch == 0x2020 || uch == 0x00b1) {
      latin_stutter += 3;
    }
    if ((uch >= 0x2000 && uch <= 0x206f && uch != 0x3000)) {
      ++latin_stutter;
    }
    if (latin_stutter > 800) {
      break;
    }
    p = g_utf8_next_char(p);
  }
  if (jp_mass >= 8 && jp_mass * 3 >= latin_stutter) {
    return false;
  }
  const glong thresh = pick.size() / 60 + static_cast<glong>(54);
  const bool noisy = latin_stutter >= thresh || latin_stutter >= 175;
  if (noisy) {
    logf("clipboard: pick resembles latin mojibake (latin_score=%ld, "
         "jp_glyphs≈%ld, len=%zu) — "
         "block Mozc on this blob",
         static_cast<long>(latin_stutter), static_cast<long>(jp_mass),
         pick.size());
  }
  return noisy;
}

bool mojibake_recovery_aggressive_enabled() {
  const char *v = std::getenv("MODORE_MOJIBAKE_RECOVERY");
  return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

} // namespace modore_host
