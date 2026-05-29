// modore — bridge implementation.
//
// Drives MozcDirectClient (in-process, no daemon) to convert a romaji span
// into its top-candidate Japanese form. Wire shape:
//
//   for each byte c in romaji:
//     SendKey(KeyEvent{key_code = c})       // build the preedit
//   SendKey(KeyEvent{special_key = SPACE})  // trigger conversion
//   // [optionally capture candidate_window here for cycle/undo features]
//   SendKey(KeyEvent{special_key = ENTER})  // commit top candidate
//   -> output.result().value()
//
// The base loop always commits the top candidate. Frontends that want
// candidate cycling (the conversion hotkey pressed repeatedly within a window) call
// the `_with_candidates_ex` variant, which captures the top-N candidate
// list from the post-SPACE response before the ENTER call overwrites the
// output. The list is returned alongside the committed string so the
// frontend can replace the AX-visible text directly without paying for
// another round-trip per cycle press.

#include "mozc_bridge.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <cstdlib>

#include "backend_iface.h"

namespace {

thread_local std::string g_last_error;

std::mutex g_init_mutex;
std::mutex g_convert_mutex;
// Read on the convert paths and written by init/shutdown under different
// mutexes; atomic so that cross-mutex read/write is not a data race.
std::atomic<bool> g_initialized{false};
std::unique_ptr<modore::mozc_bridge::Backend> g_backend;

extern "C" void mozc_bridge_set_error(const char *msg) {
    g_last_error.assign(msg ? msg : "");
}

extern "C" void mozc_bridge_clear_error(void) {
    g_last_error.clear();
}

}  // namespace

extern "C" int mozc_bridge_init(const char *user_profile_dir) {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_initialized) {
        mozc_bridge_clear_error();
        return 0;
    }

    const char *requested_backend = std::getenv("MODORE_MOZC_BACKEND");
    std::string error;
    g_backend = modore::mozc_bridge::CreateBackend(
        requested_backend, user_profile_dir, &error);
    if (!g_backend) {
        mozc_bridge_set_error(error.c_str());
        return -1;
    }

    g_initialized = true;
    mozc_bridge_clear_error();
    return 0;
}

extern "C" int mozc_bridge_convert(const char *romaji,
                                   size_t romaji_len,
                                   char *out_buf,
                                   size_t out_cap,
                                   size_t *out_len) {
    return mozc_bridge_convert_ex(romaji, romaji_len, out_buf, out_cap, out_len,
                                  /*flags=*/0u);
}

extern "C" int mozc_bridge_convert_ex(const char *romaji,
                                      size_t romaji_len,
                                      char *out_buf,
                                      size_t out_cap,
                                      size_t *out_len,
                                      unsigned int flags) {
    return mozc_bridge_convert_with_candidates_ex(
        romaji, romaji_len,
        out_buf, out_cap, out_len,
        /*cands_buf=*/nullptr, /*cands_cap=*/0,
        /*cands_total_len=*/nullptr,
        /*max_candidates=*/0,
        /*out_candidate_count=*/nullptr,
        flags);
}

extern "C" int mozc_bridge_convert_with_candidates_ex(
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
    unsigned int flags) {
    // Hold g_convert_mutex across the g_backend check *and* use. shutdown
    // takes the same lock before resetting g_backend, so the backend can't be
    // destroyed between the null-check and the call (UAF on the macOS
    // terminate-vs-convert race).
    std::lock_guard<std::mutex> lock(g_convert_mutex);
    if (!g_initialized || !g_backend) {
        mozc_bridge_set_error("mozc_bridge_init has not been called");
        return -1;
    }
    std::string error;
    const int rc = g_backend->ConvertWithCandidatesEx(
        romaji, romaji_len,
        commit_buf, commit_cap, commit_len,
        cands_buf, cands_cap,
        cands_total_len,
        max_candidates,
        out_candidate_count,
        flags,
        &error);
    if (rc == 0 || rc > 0) {
        mozc_bridge_clear_error();
    } else {
        mozc_bridge_set_error(error.c_str());
    }
    return rc;
}

extern "C" int mozc_bridge_convert_with_candidate_details_ex(
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
    unsigned int flags) {
    // See mozc_bridge_convert_with_candidates_ex: g_backend check + use are
    // both under g_convert_mutex to stay exclusive with shutdown's reset.
    std::lock_guard<std::mutex> lock(g_convert_mutex);
    if (!g_initialized || !g_backend) {
        mozc_bridge_set_error("mozc_bridge_init has not been called");
        return -1;
    }
    std::string error;
    const int rc = g_backend->ConvertWithCandidateDetailsEx(
        romaji, romaji_len,
        commit_buf, commit_cap, commit_len,
        cand_records, cand_records_cap,
        cand_strings_buf, cand_strings_cap,
        cand_strings_len,
        max_candidates,
        out_candidate_count,
        flags,
        &error);
    if (rc == 0 || rc > 0) {
        mozc_bridge_clear_error();
    } else {
        mozc_bridge_set_error(error.c_str());
    }
    return rc;
}

extern "C" void mozc_bridge_shutdown(void) {
    std::lock_guard<std::mutex> init_lock(g_init_mutex);
    // Block any in-flight conversion before destroying the backend. The
    // convert paths hold g_convert_mutex across their g_backend check + use,
    // so acquiring it here can't free the backend out from under a running
    // conversion. Lock order is init→convert; convert paths take only the
    // convert mutex and init takes only the init mutex, so there's no
    // inversion / deadlock.
    std::lock_guard<std::mutex> convert_lock(g_convert_mutex);
    g_backend.reset();
    g_initialized = false;
    mozc_bridge_clear_error();
}

extern "C" const char *mozc_bridge_last_error(void) {
    return g_last_error.empty() ? nullptr : g_last_error.c_str();
}
