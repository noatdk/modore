#include "backend_candidates.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace modore::mozc_bridge {
namespace {

constexpr char kTransliterationCascadeLabel[] = "そのほかの文字種";

bool IsAsciiLike(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  for (unsigned char c : value) {
    if (c < 0x20 || c > 0x7e) {
      return false;
    }
  }
  return true;
}

bool IsUtf8InRange(std::string_view value,
                   unsigned int lo,
                   unsigned int hi) {
  if (value.empty()) {
    return false;
  }
  bool saw = false;
  for (size_t i = 0; i < value.size();) {
    unsigned char c = static_cast<unsigned char>(value[i]);
    unsigned int cp = 0;
    size_t width = 0;
    if ((c & 0x80) == 0) {
      cp = c;
      width = 1;
    } else if ((c & 0xe0) == 0xc0 && i + 1 < value.size()) {
      cp = ((c & 0x1f) << 6) |
           (static_cast<unsigned char>(value[i + 1]) & 0x3f);
      width = 2;
    } else if ((c & 0xf0) == 0xe0 && i + 2 < value.size()) {
      cp = ((c & 0x0f) << 12) |
           ((static_cast<unsigned char>(value[i + 1]) & 0x3f) << 6) |
           (static_cast<unsigned char>(value[i + 2]) & 0x3f);
      width = 3;
    } else if ((c & 0xf8) == 0xf0 && i + 3 < value.size()) {
      cp = ((c & 0x07) << 18) |
           ((static_cast<unsigned char>(value[i + 1]) & 0x3f) << 12) |
           ((static_cast<unsigned char>(value[i + 2]) & 0x3f) << 6) |
           (static_cast<unsigned char>(value[i + 3]) & 0x3f);
      width = 4;
    } else {
      return false;
    }
    if (cp < lo || cp > hi) {
      return false;
    }
    saw = true;
    i += width;
  }
  return saw;
}

unsigned int ClassifyGroup(const mozc::commands::CandidateWindow &window,
                           const std::string &value) {
  if (window.category() == mozc::commands::TRANSLITERATION) {
    if (IsAsciiLike(value)) {
      return MOZC_CANDIDATE_GROUP_ENGLISH;
    }
    if (IsUtf8InRange(value, 0x3040, 0x309f)) {
      return MOZC_CANDIDATE_GROUP_HIRAGANA;
    }
    if (IsUtf8InRange(value, 0x30a0, 0x30ff)) {
      return MOZC_CANDIDATE_GROUP_KATAKANA;
    }
    return MOZC_CANDIDATE_GROUP_TRANSLITERATION;
  }
  return MOZC_CANDIDATE_GROUP_CONVERSION;
}

CandidateEntry MakeEntry(const mozc::commands::CandidateWindow &window,
                         const mozc::commands::CandidateWindow::Candidate &cand) {
  CandidateEntry entry;
  entry.value = cand.value();
  entry.id = cand.has_id() ? cand.id() : -1;
  entry.window_category = window.category();
  entry.group = ClassifyGroup(window, entry.value);
  if (cand.has_annotation()) {
    const auto &annotation = cand.annotation();
    if (annotation.has_description()) {
      entry.description = annotation.description();
    }
    if (annotation.has_prefix()) {
      entry.prefix = annotation.prefix();
    }
    if (annotation.has_suffix()) {
      entry.suffix = annotation.suffix();
    }
  }
  return entry;
}

void FlattenCandidateWindowInto(const mozc::commands::CandidateWindow &window,
                                std::vector<CandidateEntry> *out) {
  const bool has_focused_index = window.has_focused_index() &&
                                 window.focused_index() <
                                     static_cast<uint32_t>(window.candidate_size());
  const bool skip_focused_placeholder =
      window.has_sub_candidate_window() && has_focused_index &&
      window.candidate(window.focused_index()).value() ==
          kTransliterationCascadeLabel;

  for (int i = 0; i < window.candidate_size(); ++i) {
    if (skip_focused_placeholder &&
        static_cast<uint32_t>(i) == window.focused_index()) {
      continue;
    }
    out->push_back(MakeEntry(window, window.candidate(i)));
  }

  if (window.has_sub_candidate_window()) {
    FlattenCandidateWindowInto(window.sub_candidate_window(), out);
  }
}

bool AppendStringField(const std::string &value,
                       char *cand_strings_buf,
                       size_t cand_strings_cap,
                       size_t *strings_written,
                       size_t *offset,
                       size_t *len) {
  *offset = 0;
  *len = 0;
  if (value.empty()) {
    return true;
  }
  const size_t needed = value.size() + 1;
  if (*strings_written + needed > cand_strings_cap) {
    return false;
  }
  *offset = *strings_written;
  *len = value.size();
  std::memcpy(cand_strings_buf + *strings_written, value.data(), value.size());
  cand_strings_buf[*strings_written + value.size()] = '\0';
  *strings_written += needed;
  return true;
}

}  // namespace

std::vector<CandidateEntry> FlattenCandidateWindow(
    const mozc::commands::CandidateWindow &window) {
  std::vector<CandidateEntry> out;
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

size_t CopyCandidateValuesToBuffer(const std::vector<CandidateEntry> &candidates,
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
    const std::string &value = candidates[i].value;
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

size_t CopyCandidateRecordsToBuffers(
    const std::vector<CandidateEntry> &candidates,
    mozc_bridge_candidate_record_t *cand_records,
    size_t cand_records_cap,
    char *cand_strings_buf,
    size_t cand_strings_cap,
    int max_candidates,
    int *out_candidate_count) {
  const int limit =
      (max_candidates > 0)
          ? std::min(max_candidates, static_cast<int>(candidates.size()))
          : static_cast<int>(candidates.size());
  size_t strings_written = 0;
  int count = 0;
  for (int i = 0; i < limit; ++i) {
    if (static_cast<size_t>(count) >= cand_records_cap) {
      break;
    }
    mozc_bridge_candidate_record_t record{};
    if (!AppendStringField(candidates[i].value, cand_strings_buf,
                           cand_strings_cap, &strings_written,
                           &record.value_offset, &record.value_len) ||
        !AppendStringField(candidates[i].description, cand_strings_buf,
                           cand_strings_cap, &strings_written,
                           &record.description_offset,
                           &record.description_len) ||
        !AppendStringField(candidates[i].prefix, cand_strings_buf,
                           cand_strings_cap, &strings_written,
                           &record.prefix_offset, &record.prefix_len) ||
        !AppendStringField(candidates[i].suffix, cand_strings_buf,
                           cand_strings_cap, &strings_written,
                           &record.suffix_offset, &record.suffix_len)) {
      break;
    }
    record.id = candidates[i].id;
    record.window_category = candidates[i].window_category;
    record.group = candidates[i].group;
    cand_records[count] = record;
    ++count;
  }
  if (out_candidate_count) {
    *out_candidate_count = count;
  }
  return strings_written;
}

}  // namespace modore::mozc_bridge
