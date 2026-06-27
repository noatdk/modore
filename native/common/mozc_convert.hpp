#ifndef MODORE_COMMON_MOZC_CONVERT_HPP_
#define MODORE_COMMON_MOZC_CONVERT_HPP_

// Shared conversion helper for the C++ hosts (Linux + Windows). Wraps the flat
// C ABI's candidate-returning convert in the grow-and-retry loop both hosts
// need, parses the NUL-separated candidate blob, and resolves the committed
// value once (see the focused-candidate note below). Everything is UTF-8 — the
// bridge's wire format; the Windows host converts to UTF-16 at the call site.
//
// Header-only on purpose: it depends only on the bridge ABI and the standard
// library, so each host just adds native/common to its include path — no extra
// translation unit to wire into two separate build systems. The macOS host is
// Swift and keeps its own equivalent (MozcBridge.swift).

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "mozc_bridge.h"

namespace modore::common {

struct CandidateConversion {
  // Top-1 conversion to commit inline (UTF-8). Equals candidates.front() when
  // the engine offered a list; falls back to the bridge's raw committed value
  // only when the list is empty.
  std::string committed;
  // Full candidate list in Mozc's rank order, top-1 first (UTF-8). Always
  // contains `committed` as its first element when `committed` is non-empty.
  std::vector<std::string> candidates;
};

// Candidates requested from the bridge — sized to the cycle ring / candidate
// panel both hosts surface.
inline constexpr int kDefaultMaxCandidates = 16;

// Convert `romaji_utf8` and capture the candidate list. Returns false and sets
// *error (when non-null) on failure. `flags` is the bridge MOZC_CONVERT_FLAG_*
// bitset (e.g. katakana); `max_candidates <= 0` means "as many as fit".
inline bool convert_with_candidates(const std::string &romaji_utf8,
                                    unsigned int flags, int max_candidates,
                                    CandidateConversion *out,
                                    std::string *error) {
  if (!out) {
    if (error) *error = "convert_with_candidates: null output";
    return false;
  }

  // The candidate blob is bounded by max_candidates, so a fixed, generous
  // buffer avoids a second round-trip. The bridge truncates silently to what
  // fits, which is fine — only max_candidates are surfaced anyway.
  constexpr size_t kCandidateBufferBytes = 16 * 1024;
  std::string cand_buf(kCandidateBufferBytes, '\0');

  size_t commit_cap = std::max<size_t>(romaji_utf8.size() * 4 + 64, 256);
  for (;;) {
    std::string commit_buf(commit_cap, '\0');
    size_t commit_len = 0;
    size_t cand_total_len = 0;
    int candidate_count = 0;
    const int rc = mozc_bridge_convert_with_candidates_ex(
        romaji_utf8.data(), romaji_utf8.size(), commit_buf.data(),
        commit_buf.size(), &commit_len, cand_buf.data(), cand_buf.size(),
        &cand_total_len, max_candidates, &candidate_count, flags);
    if (rc == 0) {
      out->candidates.clear();
      size_t pos = 0;
      while (pos < cand_total_len) {
        size_t next = pos;
        while (next < cand_total_len && cand_buf[next] != '\0') ++next;
        out->candidates.emplace_back(cand_buf.data() + pos, next - pos);
        pos = next + 1;
      }
      // Mozc's committed value is its *focused* candidate, and the bridge
      // advances the focus one step — the extra SPACE it sends to open the
      // candidate window moves the highlight off the top — before committing.
      // So the bridge's commit is the second entry, while the candidate list
      // stays in rank order with the real top-1 at front. Commit
      // candidates.front(); otherwise the second candidate shows first and the
      // top is reachable only by cycling. Fall back to the bridge value (and
      // seed the list with it) only when the engine returned no list.
      if (!out->candidates.empty()) {
        out->committed = out->candidates.front();
      } else {
        out->committed.assign(commit_buf.data(), commit_len);
        if (!out->committed.empty()) {
          out->candidates.push_back(out->committed);
        }
      }
      (void)candidate_count;
      return true;
    }
    if (rc < 0) {
      if (error) {
        const char *e = mozc_bridge_last_error();
        *error = e ? e : "mozc_bridge_convert_with_candidates_ex failed";
      }
      return false;
    }
    if (static_cast<size_t>(rc) > (1u << 20)) {
      if (error) *error = "mozc convert: unreasonably large output";
      return false;
    }
    commit_cap = static_cast<size_t>(rc) + 1;
  }
}

}  // namespace modore::common

#endif  // MODORE_COMMON_MOZC_CONVERT_HPP_
