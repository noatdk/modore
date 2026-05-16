#ifndef MODORE_MOZC_BRIDGE_BACKEND_IFACE_H_
#define MODORE_MOZC_BRIDGE_BACKEND_IFACE_H_

#include <cstddef>
#include <memory>
#include <string>

namespace modore::mozc_bridge {

// Internal backend contract behind the flat C ABI in mozc_bridge.h.
// Backends are responsible for their own engine/client initialization.
class Backend {
 public:
  virtual ~Backend() = default;

  virtual int ConvertWithCandidatesEx(const char *romaji,
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
                                      std::string *error) = 0;
};

// Creates the current default backend for this build/platform.
//
// `requested_backend` is reserved for backend selection experiments. Empty /
// null means "default". Unknown or unsupported names return nullptr and fill
// `error`.
std::unique_ptr<Backend> CreateBackend(const char *requested_backend,
                                       const char *user_profile_dir,
                                       std::string *error);

#ifdef __APPLE__
std::unique_ptr<Backend> CreateGoogleImeMacBackend(std::string *error);
#endif

}  // namespace modore::mozc_bridge

#endif  // MODORE_MOZC_BRIDGE_BACKEND_IFACE_H_
