// mozc_session.cpp — Mozc bridge calls and conversion-session state.

#include "host_internal.hpp"
#include "mozc_convert.hpp"

namespace modore_host {

bool mozc_convert_utf8(const std::string &romaji, std::string *replacement) {
  if (romaji.empty()) {
    return false;
  }
  size_t cap = std::max<size_t>(romaji.size() * 4 + 64, 256);
  for (;;) {
    std::string buf(cap, '\0');
    size_t out_len = 0;
    int rc = mozc_bridge_convert(romaji.data(), romaji.size(), buf.data(),
                                 buf.size(), &out_len);
    if (rc == 0) {
      replacement->assign(buf.data(), out_len);
      if (*replacement == romaji) {
        logf("mozc: output identical to input — engine did not transform this "
             "span");
      }
      return true;
    }
    if (rc < 0) {
      logf("mozc_bridge_convert: %s", mozc_bridge_last_error()
                                          ? mozc_bridge_last_error()
                                          : "unknown error");
      return false;
    }
    if (static_cast<size_t>(rc) > (1u << 20)) {
      logf("mozc_bridge_convert: unreasonably large output (%d)", rc);
      return false;
    }
    cap = static_cast<size_t>(rc) + 1;
  }
}

std::optional<std::pair<std::string, std::vector<std::string>>>
mozc_convert_utf8_with_candidates(const std::string &romaji) {
  if (romaji.empty()) {
    return std::nullopt;
  }
  modore::common::CandidateConversion conv;
  std::string error;
  if (!modore::common::convert_with_candidates(
          romaji, /*flags=*/0u, modore::common::kDefaultMaxCandidates, &conv,
          &error)) {
    logf("mozc_bridge_convert_with_candidates_ex: %s",
         error.empty() ? "unknown error" : error.c_str());
    return std::nullopt;
  }
  return std::make_pair(std::move(conv.committed), std::move(conv.candidates));
}

void clear_conversion_session_locked() {
  if (g_has_conversion_session && g_conversion_session.focus) {
    g_object_unref(g_conversion_session.focus);
    g_conversion_session.focus = nullptr;
  }
  g_conversion_session = ConversionSession{};
  g_has_conversion_session = false;
}

std::vector<std::string>
normalize_candidate_session_state(const std::string &replacement,
                                  std::vector<std::string> candidates,
                                  int *current_index) {
  if (candidates.empty()) {
    candidates.push_back(replacement);
    if (current_index) {
      *current_index = 0;
    }
    return candidates;
  }
  const auto it = std::find(candidates.begin(), candidates.end(), replacement);
  if (it == candidates.end()) {
    candidates.insert(candidates.begin(), replacement);
    if (current_index) {
      *current_index = 0;
    }
    return candidates;
  }
  if (current_index) {
    *current_index = static_cast<int>(std::distance(candidates.begin(), it));
  }
  return candidates;
}

void invalidate_conversion_session_for_user_input() {
  std::lock_guard<std::mutex> lock(g_conversion_session_mu);
  if (!g_has_conversion_session) {
    return;
  }
  modore_log("session", "session cleared: user typed a non-hotkey key");
  clear_conversion_session_locked();
}

void set_conversion_session(ConversionSession session) {
  std::lock_guard<std::mutex> lock(g_conversion_session_mu);
  clear_conversion_session_locked();
  modore_log("cycle",
             "session stored: backing=%s app_id=%s candidates=%zu index=%d",
             session.backing == ConversionSession::Backing::AtspiEditable
                 ? "atspi"
                 : "clipboard",
             session.app_id.empty() ? "(unset)" : session.app_id.c_str(),
             session.candidates.size(), session.candidate_index);
  g_conversion_session = std::move(session);
  g_has_conversion_session = true;
}

bool conversion_session_available_locked(
    std::chrono::steady_clock::time_point now) {
  if (!g_has_conversion_session) {
    return false;
  }
  constexpr auto kSessionTtl = std::chrono::milliseconds(5000);
  if (now - g_conversion_session.last_touch > kSessionTtl) {
    clear_conversion_session_locked();
    return false;
  }
  return true;
}

glong clipboard_cycle_residue_chars() {
  if (!hotkey_can_leak_text(g_conversion_hotkey_keysym)) {
    return 0;
  }
  const unsigned int held_mods = evdev_current_modifier_mask();
  if ((held_mods & g_conversion_hotkey_modifier_mask) == 0) {
    return 0;
  }
  return 1;
}

} // namespace modore_host
