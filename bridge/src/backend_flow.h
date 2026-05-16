#ifndef MODORE_MOZC_BRIDGE_BACKEND_FLOW_H_
#define MODORE_MOZC_BRIDGE_BACKEND_FLOW_H_

#include <cstddef>
#include <string>
#include <vector>

#include "backend_candidates.h"
#include "protocol/commands.pb.h"

namespace modore::mozc_bridge {

class SessionDriver {
 public:
  virtual ~SessionDriver() = default;

  virtual bool Begin(mozc::commands::Output *out, std::string *error) = 0;
  virtual bool SendKey(const mozc::commands::KeyEvent &key,
                       mozc::commands::Output *out,
                       const char *step,
                       std::string *error) = 0;
  virtual bool HighlightCandidate(int id,
                                  mozc::commands::Output *out,
                                  std::string *error) = 0;
  virtual bool Submit(mozc::commands::Output *out, std::string *error) = 0;
  virtual void Finish() = 0;
};

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
                   std::string *error);

std::vector<CandidateEntry> CaptureFocusedSegmentCandidates(
    SessionDriver *driver,
    mozc::commands::Output *out,
    std::string *error);

std::vector<CandidateEntry> RebuildFullSpanCandidates(
    const mozc::commands::Output &base_output,
    const std::vector<CandidateEntry> &focused_segment_candidates);

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
    std::string *error);

}  // namespace modore::mozc_bridge

#endif  // MODORE_MOZC_BRIDGE_BACKEND_FLOW_H_
