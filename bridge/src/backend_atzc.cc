// backend_atzc.cc — conversion backend that relays to a Wine-hosted Japanese
// engine through atzcd over the atzc Unix-socket protocol (libatzcclient).
//
// Selected with MODORE_MOZC_BACKEND=atzc (the Linux host sets this from
// `[bridge] mozc_backend = atzc`). Built only when MODORE_ENABLE_ATZC is
// defined; see bridge/CMakeLists.txt and `make fetch-atzc`. When the backend
// is not compiled in, CreateBackend() reports "unknown mozc backend: atzc".
//
// Pickup uses convert (henkan / fast top-1); candidate prefetch and cycle use
// candidates (full list). Both map to atzcd's split socket protocol.

#include "backend_iface.h"
#include "mozc_bridge.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "atzc/client.h"

namespace modore::mozc_bridge {
namespace {

// Copy a UTF-8 commit string into the caller buffer using the bridge contract
// (mozc_bridge.h): 0 on success with *len set; the required size (> cap) if the
// buffer is too small, leaving the buffer and *len untouched.
int CopyCommit(const std::string &commit, char *buf, size_t cap, size_t *len) {
  if (commit.size() > cap) {
    return static_cast<int>(commit.size());
  }
  if (!commit.empty()) {
    std::memcpy(buf, commit.data(), commit.size());
  }
  if (len) {
    *len = commit.size();
  }
  return 0;
}

// NUL-separated candidate values, mirroring CopyCandidateValuesToBuffer:
// each value followed by '\0'; silent truncation when the buffer fills;
// max_candidates <= 0 means "as many as fit". Returns bytes written.
size_t PackValues(const std::vector<std::string> &values, char *buf, size_t cap,
                  int max_candidates, int *out_count) {
  const int limit = (max_candidates > 0)
                        ? std::min(max_candidates, static_cast<int>(values.size()))
                        : static_cast<int>(values.size());
  size_t written = 0;
  int count = 0;
  for (int i = 0; i < limit; ++i) {
    const std::string &v = values[i];
    const size_t needed = v.size() + 1;
    if (written + needed > cap) {
      break;
    }
    std::memcpy(buf + written, v.data(), v.size());
    buf[written + v.size()] = '\0';
    written += needed;
    ++count;
  }
  if (out_count) {
    *out_count = count;
  }
  return written;
}

// Structured records, mirroring CopyCandidateRecordsToBuffers. atzc carries only
// candidate values, so description/prefix/suffix stay length 0. Returns bytes
// written into the shared string blob.
size_t PackRecords(const std::vector<std::string> &values,
                   mozc_bridge_candidate_record_t *records, size_t records_cap,
                   char *strings_buf, size_t strings_cap, int max_candidates,
                   int *out_count) {
  const int limit = (max_candidates > 0)
                        ? std::min(max_candidates, static_cast<int>(values.size()))
                        : static_cast<int>(values.size());
  size_t written = 0;
  int count = 0;
  for (int i = 0; i < limit; ++i) {
    if (static_cast<size_t>(count) >= records_cap) {
      break;
    }
    const std::string &v = values[i];
    mozc_bridge_candidate_record_t record{};
    if (!v.empty()) {
      const size_t needed = v.size() + 1;
      if (written + needed > strings_cap) {
        break;
      }
      record.value_offset = written;
      record.value_len = v.size();
      std::memcpy(strings_buf + written, v.data(), v.size());
      strings_buf[written + v.size()] = '\0';
      written += needed;
    }
    record.id = -1;
    record.group = MOZC_CANDIDATE_GROUP_CONVERSION;
    records[count] = record;
    ++count;
  }
  if (out_count) {
    *out_count = count;
  }
  return written;
}

class AtzcBackend final : public Backend {
 public:
  int ConvertWithCandidatesEx(const char *romaji,
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
                              std::string *error) override {
    // KATAKANA: the atzc engine has no katakana mode and the Linux host never
    // sets this bit, so it is accepted and ignored (top-1 kanji is committed).
    (void)flags;
    atzc::ConvertResult result;
    const bool need_candidates = cands_buf != nullptr && cands_cap > 0;
    if (!Convert(std::string(romaji, romaji_len), max_candidates,
                 need_candidates, &result, error)) {
      return -1;
    }
    const int rc = CopyCommit(result.commit, commit_buf, commit_cap, commit_len);
    if (rc != 0) {
      return rc;  // too small (or commit error) — candidate output undefined
    }
    if (need_candidates) {
      const size_t total = PackValues(result.candidates, cands_buf, cands_cap,
                                      max_candidates, out_candidate_count);
      if (cands_total_len) {
        *cands_total_len = total;
      }
    } else {
      if (cands_total_len) {
        *cands_total_len = 0;
      }
      if (out_candidate_count) {
        *out_candidate_count = 0;
      }
    }
    return 0;
  }

  int ConvertWithCandidateDetailsEx(const char *romaji,
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
                                    std::string *error) override {
    (void)flags;
    atzc::ConvertResult result;
    const bool need_candidates =
        cand_records != nullptr && cand_records_cap > 0 &&
        cand_strings_buf != nullptr && cand_strings_cap > 0;
    if (!Convert(std::string(romaji, romaji_len), max_candidates,
                 need_candidates, &result, error)) {
      return -1;
    }
    const int rc = CopyCommit(result.commit, commit_buf, commit_cap, commit_len);
    if (rc != 0) {
      return rc;
    }
    if (!need_candidates) {
      if (out_candidate_count) {
        *out_candidate_count = 0;
      }
      if (cand_strings_len) {
        *cand_strings_len = 0;
      }
      return 0;
    }
    const size_t total =
        PackRecords(result.candidates, cand_records, cand_records_cap,
                    cand_strings_buf, cand_strings_cap, max_candidates,
                    out_candidate_count);
    if (cand_strings_len) {
      *cand_strings_len = total;
    }
    return 0;
  }

 private:
  // Lazy-connect plus one reconnect on failure (atzcd may have restarted) —
  // the same pattern the fcitx5 addon uses. The bridge serializes every
  // conversion under a mutex, so a single Client instance is safe.
  bool Convert(const std::string &romaji,
               int max_candidates,
               bool need_candidates,
               atzc::ConvertResult *out,
               std::string *error) {
    const int cap = max_candidates > 0 ? max_candidates : 0;  // 0 = no cap
    std::string err;
    auto run = [&](bool full) -> bool {
      if (!client_.connected() && !client_.connect(&err)) {
        return false;
      }
      if (full) {
        return client_.candidates(romaji, cap, out, &err);
      }
      return client_.convert(romaji, out, &err);
    };
    if (run(need_candidates)) {
      return true;
    }
    client_.close();
    if (client_.connect(&err) && run(need_candidates)) {
      return true;
    }
    *error = "atzc: " + (err.empty() ? std::string("conversion failed") : err);
    return false;
  }

  atzc::Client client_;
};

}  // namespace

std::unique_ptr<Backend> CreateAtzcBackend(std::string *error) {
  // Connection is deferred to the first conversion: atzcd may still be warming
  // the engine when the host starts, and host startup must not block on it.
  (void)error;
  return std::make_unique<AtzcBackend>();
}

}  // namespace modore::mozc_bridge
