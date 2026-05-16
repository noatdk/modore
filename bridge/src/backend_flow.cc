#include "backend_flow.h"

#include "mozc_bridge.h"

#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace modore::mozc_bridge {
namespace {

std::string JoinPreeditSegments(const mozc::commands::Output &out) {
  std::string s;
  if (!out.has_preedit()) {
    return s;
  }
  for (int i = 0; i < out.preedit().segment_size(); ++i) {
    s += out.preedit().segment(i).value();
  }
  return s;
}

std::vector<std::string> PreeditSegments(const mozc::commands::Output &out) {
  std::vector<std::string> segments;
  if (!out.has_preedit()) {
    return segments;
  }
  segments.reserve(out.preedit().segment_size());
  for (int i = 0; i < out.preedit().segment_size(); ++i) {
    segments.push_back(out.preedit().segment(i).value());
  }
  return segments;
}

std::string JoinSegments(const std::vector<std::string> &segments) {
  std::string out;
  for (const std::string &segment : segments) {
    out += segment;
  }
  return out;
}

void AppendUnique(std::vector<CandidateEntry> *out,
                  std::unordered_set<std::string> *seen,
                  CandidateEntry entry) {
  if (entry.value.empty()) {
    return;
  }
  if (seen->insert(entry.value).second) {
    out->push_back(std::move(entry));
  }
}

}  // namespace

std::vector<CandidateEntry> CaptureFocusedSegmentCandidates(
    SessionDriver *driver,
    mozc::commands::Output *out,
    std::string *error) {
  if (!out->has_candidate_window()) {
    return {};
  }

  std::vector<CandidateEntry> candidates =
      FlattenCandidateWindow(out->candidate_window());
  if (out->candidate_window().has_sub_candidate_window()) {
    return candidates;
  }
  if (out->candidate_window().candidate_size() == 0 ||
      !out->candidate_window().candidate(0).has_id()) {
    return candidates;
  }
  const int original_id = out->candidate_window().candidate(0).id();

  const int placeholder =
      FindTransliterationPlaceholder(out->candidate_window());
  if (placeholder < 0 || !out->candidate_window().has_focused_index()) {
    return candidates;
  }
  if (!out->candidate_window().candidate(placeholder).has_id()) {
    return candidates;
  }
  if (!driver->HighlightCandidate(
          out->candidate_window().candidate(placeholder).id(), out, error)) {
    return candidates;
  }
  if (out->has_candidate_window() && out->candidate_window().has_sub_candidate_window()) {
    candidates = FlattenCandidateWindow(out->candidate_window());
  }
  (void)driver->HighlightCandidate(original_id, out, error);
  return candidates;
}

std::vector<CandidateEntry> RebuildFullSpanCandidates(
    const mozc::commands::Output &base_output,
    const std::vector<CandidateEntry> &focused_segment_candidates) {
  std::vector<CandidateEntry> full_candidates;
  std::unordered_set<std::string> seen;

  const std::vector<std::string> segments = PreeditSegments(base_output);
  const std::string base = JoinSegments(segments);
  CandidateEntry base_entry;
  base_entry.value = base;
  base_entry.window_category = mozc::commands::CONVERSION;
  base_entry.group = MOZC_CANDIDATE_GROUP_CONVERSION;
  AppendUnique(&full_candidates, &seen, std::move(base_entry));

  if (segments.empty() || focused_segment_candidates.empty()) {
    return full_candidates;
  }

  int focused_segment = 0;
  if (base_output.preedit().has_highlighted_position()) {
    focused_segment = static_cast<int>(base_output.preedit().highlighted_position());
  }
  if (focused_segment < 0 ||
      focused_segment >= static_cast<int>(segments.size())) {
    focused_segment = 0;
  }

  for (const CandidateEntry &candidate : focused_segment_candidates) {
    std::vector<std::string> rebuilt = segments;
    rebuilt[focused_segment] = candidate.value;
    CandidateEntry entry = candidate;
    entry.value = JoinSegments(rebuilt);
    AppendUnique(&full_candidates, &seen, std::move(entry));
  }
  return full_candidates;
}

int RunConvertFlow(SessionDriver *driver,
                   const char *romaji,
                   size_t romaji_len,
                   char *commit_buf,
                   size_t commit_cap,
                   size_t *commit_len,
                   char *cands_buf,
                   size_t cands_cap,
                   size_t *cands_total_len,
                   int max_candidates,
                   int *out_candidate_count,
                   unsigned int flags,
                   std::string *error) {
  if (!romaji || !commit_len) {
    *error = "null pointer passed to mozc_bridge_convert";
    return -1;
  }
  if (cands_total_len) {
    *cands_total_len = 0;
  }
  if (out_candidate_count) {
    *out_candidate_count = 0;
  }
  if (romaji_len == 0) {
    *commit_len = 0;
    error->clear();
    return 0;
  }

  mozc::commands::Output out;
  if (!driver->Begin(&out, error)) {
    return -1;
  }

  const bool force_katakana = (flags & MOZC_CONVERT_FLAG_KATAKANA) != 0u;
  const bool capture_cands =
      (cands_buf != nullptr) && (cands_cap > 0) && !force_katakana;

  int rc = 0;
  do {
    for (size_t i = 0; i < romaji_len; ++i) {
      mozc::commands::KeyEvent key;
      key.set_key_code(static_cast<unsigned char>(romaji[i]));
      if (!driver->SendKey(key, &out, "romaji", error)) {
        rc = -1;
        break;
      }
    }
    if (rc != 0) {
      break;
    }

    {
      mozc::commands::KeyEvent key;
      key.set_special_key(mozc::commands::KeyEvent::SPACE);
      if (!driver->SendKey(key, &out, "space", error)) {
        rc = -1;
        break;
      }
    }

    if (force_katakana) {
      mozc::commands::KeyEvent key;
      key.set_special_key(mozc::commands::KeyEvent::F7);
      if (!driver->SendKey(key, &out, "f7", error)) {
        rc = -1;
        break;
      }
    } else if (capture_cands && !out.has_candidate_window()) {
      mozc::commands::KeyEvent key;
      key.set_special_key(mozc::commands::KeyEvent::SPACE);
      if (!driver->SendKey(key, &out, "space-candidates", error)) {
        rc = -1;
        break;
      }
    }

    if (!force_katakana && capture_cands && out.has_candidate_window()) {
      const mozc::commands::Output base_output = out;
      const std::vector<CandidateEntry> focused_candidates =
          CaptureFocusedSegmentCandidates(driver, &out, error);
      const std::vector<CandidateEntry> full_candidates =
          RebuildFullSpanCandidates(base_output, focused_candidates);
      const size_t written = CopyCandidateValuesToBuffer(
          full_candidates, cands_buf, cands_cap, max_candidates,
          out_candidate_count);
      if (cands_total_len) {
        *cands_total_len = written;
      }
    }

    std::string committed;
    if (force_katakana) {
      committed = JoinPreeditSegments(out);
    } else {
      if (!driver->Submit(&out, error)) {
        rc = -1;
        break;
      }
      if (out.has_result()) {
        committed = out.result().value();
      } else {
        committed = JoinPreeditSegments(out);
      }
    }

    if (committed.empty()) {
      committed.assign(romaji, romaji_len);
    }
    if (committed.size() > commit_cap) {
      rc = static_cast<int>(committed.size());
      break;
    }
    if (commit_buf) {
      std::memcpy(commit_buf, committed.data(), committed.size());
    }
    *commit_len = committed.size();
    error->clear();
    rc = 0;
  } while (false);

  driver->Finish();
  return rc;
}

int RunConvertFlowWithDetails(
    SessionDriver *driver,
    const char *romaji,
    size_t romaji_len,
    char *commit_buf,
    size_t commit_cap,
    size_t *commit_len,
    mozc_bridge_candidate_record_t *cand_records,
    size_t cand_records_cap,
    char *cand_strings_buf,
    size_t cand_strings_cap,
    size_t *cand_strings_len,
    int max_candidates,
    int *out_candidate_count,
    unsigned int flags,
    std::string *error) {
  if (!romaji || !commit_len) {
    *error = "null pointer passed to mozc_bridge_convert";
    return -1;
  }
  if (cand_strings_len) {
    *cand_strings_len = 0;
  }
  if (out_candidate_count) {
    *out_candidate_count = 0;
  }
  if (romaji_len == 0) {
    *commit_len = 0;
    error->clear();
    return 0;
  }

  mozc::commands::Output out;
  if (!driver->Begin(&out, error)) {
    return -1;
  }

  const bool force_katakana = (flags & MOZC_CONVERT_FLAG_KATAKANA) != 0u;
  const bool capture_cands =
      (cand_records != nullptr) && (cand_records_cap > 0) &&
      (cand_strings_buf != nullptr) && (cand_strings_cap > 0) &&
      !force_katakana;

  int rc = 0;
  do {
    for (size_t i = 0; i < romaji_len; ++i) {
      mozc::commands::KeyEvent key;
      key.set_key_code(static_cast<unsigned char>(romaji[i]));
      if (!driver->SendKey(key, &out, "romaji", error)) {
        rc = -1;
        break;
      }
    }
    if (rc != 0) {
      break;
    }

    {
      mozc::commands::KeyEvent key;
      key.set_special_key(mozc::commands::KeyEvent::SPACE);
      if (!driver->SendKey(key, &out, "space", error)) {
        rc = -1;
        break;
      }
    }

    if (force_katakana) {
      mozc::commands::KeyEvent key;
      key.set_special_key(mozc::commands::KeyEvent::F7);
      if (!driver->SendKey(key, &out, "f7", error)) {
        rc = -1;
        break;
      }
    } else if (capture_cands && !out.has_candidate_window()) {
      mozc::commands::KeyEvent key;
      key.set_special_key(mozc::commands::KeyEvent::SPACE);
      if (!driver->SendKey(key, &out, "space-candidates", error)) {
        rc = -1;
        break;
      }
    }

    if (!force_katakana && capture_cands && out.has_candidate_window()) {
      const mozc::commands::Output base_output = out;
      const std::vector<CandidateEntry> focused_candidates =
          CaptureFocusedSegmentCandidates(driver, &out, error);
      const std::vector<CandidateEntry> full_candidates =
          RebuildFullSpanCandidates(base_output, focused_candidates);
      const size_t written = CopyCandidateRecordsToBuffers(
          full_candidates, cand_records, cand_records_cap, cand_strings_buf,
          cand_strings_cap, max_candidates, out_candidate_count);
      if (cand_strings_len) {
        *cand_strings_len = written;
      }
    }

    std::string committed;
    if (force_katakana) {
      committed = JoinPreeditSegments(out);
    } else {
      if (!driver->Submit(&out, error)) {
        rc = -1;
        break;
      }
      if (out.has_result()) {
        committed = out.result().value();
      } else {
        committed = JoinPreeditSegments(out);
      }
    }

    if (committed.empty()) {
      committed.assign(romaji, romaji_len);
    }
    if (committed.size() > commit_cap) {
      rc = static_cast<int>(committed.size());
      break;
    }
    if (commit_buf) {
      std::memcpy(commit_buf, committed.data(), committed.size());
    }
    *commit_len = committed.size();
    error->clear();
    rc = 0;
  } while (false);

  driver->Finish();
  return rc;
}

}  // namespace modore::mozc_bridge
