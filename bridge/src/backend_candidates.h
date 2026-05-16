#ifndef MODORE_MOZC_BRIDGE_BACKEND_CANDIDATES_H_
#define MODORE_MOZC_BRIDGE_BACKEND_CANDIDATES_H_

#include <cstddef>
#include <string>
#include <vector>

#include "mozc_bridge.h"
#include "protocol/commands.pb.h"

namespace modore::mozc_bridge {

struct CandidateEntry {
  std::string value;
  std::string description;
  std::string prefix;
  std::string suffix;
  int id = -1;
  unsigned int window_category = 0;
  unsigned int group = MOZC_CANDIDATE_GROUP_UNKNOWN;
};

std::vector<CandidateEntry> FlattenCandidateWindow(
    const mozc::commands::CandidateWindow &window);

int FindTransliterationPlaceholder(
    const mozc::commands::CandidateWindow &window);

size_t CopyCandidateValuesToBuffer(
    const std::vector<CandidateEntry> &candidates,
    char *cands_buf,
    size_t cands_cap,
    int max_candidates,
    int *out_candidate_count);

size_t CopyCandidateRecordsToBuffers(
    const std::vector<CandidateEntry> &candidates,
    mozc_bridge_candidate_record_t *cand_records,
    size_t cand_records_cap,
    char *cand_strings_buf,
    size_t cand_strings_cap,
    int max_candidates,
    int *out_candidate_count);

}  // namespace modore::mozc_bridge

#endif  // MODORE_MOZC_BRIDGE_BACKEND_CANDIDATES_H_
