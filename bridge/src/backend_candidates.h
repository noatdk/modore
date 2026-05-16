#ifndef MODORE_MOZC_BRIDGE_BACKEND_CANDIDATES_H_
#define MODORE_MOZC_BRIDGE_BACKEND_CANDIDATES_H_

#include <cstddef>
#include <string>
#include <vector>

#include "protocol/commands.pb.h"

namespace modore::mozc_bridge {

std::vector<std::string> FlattenCandidateWindow(
    const mozc::commands::CandidateWindow &window);

int FindTransliterationPlaceholder(
    const mozc::commands::CandidateWindow &window);

size_t CopyCandidatesToBuffer(const std::vector<std::string> &candidates,
                              char *cands_buf,
                              size_t cands_cap,
                              int max_candidates,
                              int *out_candidate_count);

}  // namespace modore::mozc_bridge

#endif  // MODORE_MOZC_BRIDGE_BACKEND_CANDIDATES_H_
