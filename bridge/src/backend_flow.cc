#include "backend_flow.h"

#include "mozc_bridge.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
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

bool TraceRawCandidatesEnabled() {
  static const bool enabled = std::getenv("MODORE_BRIDGE_TRACE_RAW_CANDIDATES") != nullptr;
  return enabled;
}

void TraceRawCandidates(const mozc::commands::Output &out) {
  if (!TraceRawCandidatesEnabled() || !out.has_candidate_window()) {
    return;
  }
  const auto &window = out.candidate_window();
  std::fprintf(stderr, "[com.modore.bridge:raw] has_focused_index=%d focused_index=%u size=%d\n",
               window.has_focused_index() ? 1 : 0,
               window.has_focused_index() ? window.focused_index() : 0u,
               window.candidate_size());
  for (int i = 0; i < window.candidate_size(); ++i) {
    std::fprintf(stderr, "[com.modore.bridge:raw]   [%d] id=%d value=%s\n",
                 i, window.candidate(i).id(), window.candidate(i).value().c_str());
  }
  if (window.has_sub_candidate_window()) {
    const auto &sub = window.sub_candidate_window();
    std::fprintf(stderr, "[com.modore.bridge:raw] sub has_focused_index=%d focused_index=%u size=%d\n",
                 sub.has_focused_index() ? 1 : 0,
                 sub.has_focused_index() ? sub.focused_index() : 0u,
                 sub.candidate_size());
    for (int i = 0; i < sub.candidate_size(); ++i) {
      std::fprintf(stderr, "[com.modore.bridge:raw]   sub[%d] id=%d value=%s\n",
                   i, sub.candidate(i).id(), sub.candidate(i).value().c_str());
    }
  }
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

namespace {

// Where a convert captures its candidate list. The flow is identical whether
// the caller wants a NUL-separated value blob (the `_with_candidates_ex` ABI)
// or structured records (the `_with_candidate_details_ex` ABI); only this sink
// differs, so the two used to be near-identical 120-line copies of the flow.
struct CandidateSink {
  virtual ~CandidateSink() = default;
  // True when the caller supplied buffers to capture into.
  virtual bool wants_capture() const = 0;
  // Serialize up to `max_candidates` of `candidates` into the caller's
  // buffers, setting `*out_candidate_count` and the caller's total-length out.
  virtual void emit(const std::vector<CandidateEntry> &candidates,
                    int max_candidates, int *out_candidate_count) = 0;
};

struct BlobSink : CandidateSink {
  BlobSink(char *b, size_t c, size_t *tl) : buf(b), cap(c), total_len(tl) {}
  bool wants_capture() const override { return buf != nullptr && cap > 0; }
  void emit(const std::vector<CandidateEntry> &candidates, int max_candidates,
            int *out_candidate_count) override {
    const size_t written = CopyCandidateValuesToBuffer(
        candidates, buf, cap, max_candidates, out_candidate_count);
    if (total_len) *total_len = written;
  }
  char *buf;
  size_t cap;
  size_t *total_len;
};

struct RecordsSink : CandidateSink {
  RecordsSink(mozc_bridge_candidate_record_t *r, size_t rc, char *s, size_t sc,
              size_t *sl)
      : records(r), records_cap(rc), strings(s), strings_cap(sc),
        strings_len(sl) {}
  bool wants_capture() const override {
    return records != nullptr && records_cap > 0 && strings != nullptr &&
           strings_cap > 0;
  }
  void emit(const std::vector<CandidateEntry> &candidates, int max_candidates,
            int *out_candidate_count) override {
    const size_t written = CopyCandidateRecordsToBuffers(
        candidates, records, records_cap, strings, strings_cap, max_candidates,
        out_candidate_count);
    if (strings_len) *strings_len = written;
  }
  mozc_bridge_candidate_record_t *records;
  size_t records_cap;
  char *strings;
  size_t strings_cap;
  size_t *strings_len;
};

// The single convert flow both public entry points share: build the preedit
// byte-by-byte, trigger conversion (SPACE), optionally force katakana (F7) or
// open the candidate window (a second SPACE), capture candidates through
// `sink`, then commit the top candidate. The caller's length out param is
// reset by the wrapper before this runs.
int RunConvertFlowImpl(SessionDriver *driver, const char *romaji,
                       size_t romaji_len, char *commit_buf, size_t commit_cap,
                       size_t *commit_len, CandidateSink *sink,
                       int max_candidates, int *out_candidate_count,
                       unsigned int flags, std::string *error) {
  if (!romaji || !commit_len) {
    *error = "null pointer passed to mozc_bridge_convert";
    return -1;
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
  const bool capture_cands = sink->wants_capture() && !force_katakana;

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
      TraceRawCandidates(out);
      const mozc::commands::Output base_output = out;
      const std::vector<CandidateEntry> focused_candidates =
          CaptureFocusedSegmentCandidates(driver, &out, error);
      const std::vector<CandidateEntry> full_candidates =
          RebuildFullSpanCandidates(base_output, focused_candidates);
      sink->emit(full_candidates, max_candidates, out_candidate_count);
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

}  // namespace

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
  if (cands_total_len) {
    *cands_total_len = 0;
  }
  BlobSink sink(cands_buf, cands_cap, cands_total_len);
  return RunConvertFlowImpl(driver, romaji, romaji_len, commit_buf, commit_cap,
                            commit_len, &sink, max_candidates,
                            out_candidate_count, flags, error);
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
  if (cand_strings_len) {
    *cand_strings_len = 0;
  }
  RecordsSink sink(cand_records, cand_records_cap, cand_strings_buf,
                   cand_strings_cap, cand_strings_len);
  return RunConvertFlowImpl(driver, romaji, romaji_len, commit_buf, commit_cap,
                            commit_len, &sink, max_candidates,
                            out_candidate_count, flags, error);
}

}  // namespace modore::mozc_bridge
