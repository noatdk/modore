#include "backend_candidates.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace modore::mozc_bridge {
namespace {

constexpr char kTransliterationCascadeLabel[] = "そのほかの文字種";

void FlattenCandidateWindowInto(const mozc::commands::CandidateWindow &window,
                                std::vector<std::string> *out) {
  const bool skip_focused_placeholder =
      window.has_sub_candidate_window() && window.has_focused_index() &&
      window.focused_index() < static_cast<uint32_t>(window.candidate_size()) &&
      window.candidate(window.focused_index()).value() ==
          kTransliterationCascadeLabel;

  for (int i = 0; i < window.candidate_size(); ++i) {
    if (skip_focused_placeholder &&
        static_cast<uint32_t>(i) == window.focused_index()) {
      continue;
    }
    out->push_back(window.candidate(i).value());
  }

  if (window.has_sub_candidate_window()) {
    FlattenCandidateWindowInto(window.sub_candidate_window(), out);
  }
}

}  // namespace

std::vector<std::string> FlattenCandidateWindow(
    const mozc::commands::CandidateWindow &window) {
  std::vector<std::string> out;
  out.reserve(window.candidate_size() +
              (window.has_sub_candidate_window()
                   ? window.sub_candidate_window().candidate_size()
                   : 0));
  FlattenCandidateWindowInto(window, &out);
  return out;
}

int FindTransliterationPlaceholder(
    const mozc::commands::CandidateWindow &window) {
  for (int i = 0; i < window.candidate_size(); ++i) {
    if (window.candidate(i).value() == kTransliterationCascadeLabel) {
      return i;
    }
  }
  return -1;
}

size_t CopyCandidatesToBuffer(const std::vector<std::string> &candidates,
                              char *cands_buf,
                              size_t cands_cap,
                              int max_candidates,
                              int *out_candidate_count) {
  const int limit =
      (max_candidates > 0)
          ? std::min(max_candidates, static_cast<int>(candidates.size()))
          : static_cast<int>(candidates.size());
  size_t written = 0;
  int count = 0;
  for (int i = 0; i < limit; ++i) {
    const std::string &value = candidates[i];
    const size_t needed = value.size() + 1;
    if (written + needed > cands_cap) {
      break;
    }
    std::memcpy(cands_buf + written, value.data(), value.size());
    cands_buf[written + value.size()] = '\0';
    written += needed;
    ++count;
  }
  if (out_candidate_count) {
    *out_candidate_count = count;
  }
  return written;
}

}  // namespace modore::mozc_bridge
