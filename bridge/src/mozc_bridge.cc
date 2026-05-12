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
// candidate cycling (Ctrl+/ pressed repeatedly within a window) call
// the `_with_candidates_ex` variant, which captures the top-N candidate
// list from the post-SPACE response before the ENTER call overwrites the
// output. The list is returned alongside the committed string so the
// frontend can replace the AX-visible text directly without paying for
// another round-trip per cycle press.

#include "mozc_bridge.h"

#include <errno.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include <sys/stat.h>

#include "absl/log/initialize.h"
#include "base/system_util.h"
#include "config/config_handler.h"
#include "direct_client.h"
#include "protocol/commands.pb.h"

namespace {

thread_local std::string g_last_error;

std::mutex g_init_mutex;
bool g_initialized = false;
std::unique_ptr<modore::mozc_bridge::MozcDirectClient> g_client;

void set_error(const char *msg) { g_last_error.assign(msg ? msg : ""); }
void set_error(const std::string &msg) { g_last_error = msg; }
void clear_error() { g_last_error.clear(); }

// Mozc calls SetUserProfileDirectory which bypasses the usual GetDir()-time
// CreateDirectory logic; ~/.local/state/... may not exist yet.
static void mkdir_posix_path_parents(const std::string& absolute_dir) {
  if (absolute_dir.empty() || absolute_dir[0] != '/') {
    return;
  }
  for (size_t i = 1; i <= absolute_dir.size(); ++i) {
    if (i == absolute_dir.size() || absolute_dir[i] == '/') {
      std::string part = absolute_dir.substr(0, i);
      if (part.size() <= 1) {
        continue;
      }
      if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) {
        // Ignore; downstream writes surface their own failures.
      }
    }
  }
}

// Send a single key event; returns false (and sets last_error) on failure.
bool send_key(modore::mozc_bridge::MozcDirectClient *client,
              const mozc::commands::KeyEvent &key,
              mozc::commands::Output *out,
              const char *step) {
    if (!client->SendKeyWithContext(
            key, mozc::commands::Context::default_instance(), out)) {
        std::string m = std::string("SendKey failed at step: ") + step;
        set_error(m);
        return false;
    }
    return true;
}

void reset_session_best_effort(modore::mozc_bridge::MozcDirectClient *client) {
    mozc::commands::SessionCommand reset;
    reset.set_type(mozc::commands::SessionCommand::RESET_CONTEXT);
    mozc::commands::Output tail;
    (void)client->SendCommandWithContext(reset, mozc::commands::Context::default_instance(),
                                         &tail);
}

}  // namespace

extern "C" int mozc_bridge_init(const char *user_profile_dir) {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_initialized) {
        clear_error();
        return 0;
    }

    // Mozc resolves `user://config1.db` via SystemUtil::GetUserProfileDirectory(),
    // not via MOZC_USER_PROFILE_DIRECTORY (that env is unused on Linux in this
    // tree). Set the directory before any Mozc singleton touches ConfigHandler.
    if (user_profile_dir && *user_profile_dir) {
        const std::string p(user_profile_dir);
        mkdir_posix_path_parents(p);
        setenv("MOZC_USER_PROFILE_DIRECTORY", p.c_str(), /*overwrite=*/1);
        mozc::SystemUtil::SetUserProfileDirectory(p);
    }

    // Quiet absl's flag-parse log spam. Calls without flags init are fine
    // for embedded use.
    absl::InitializeLog();

    try {
        g_client =
            std::make_unique<modore::mozc_bridge::MozcDirectClient>();
    } catch (const std::exception &e) {
        set_error(std::string("MozcDirectClient ctor threw: ") + e.what());
        return -1;
    } catch (...) {
        set_error("MozcDirectClient ctor threw unknown exception");
        return -1;
    }

    // When config1.db is missing, Reload() keeps an empty protobuf and
    // NormalizeConfig() does not restore CreateDefaultConfig()'s character-form
    // rules — conversion can silently misbehave. Seed defaults once when the
    // loaded config looks uninitialized.
    //
    // Also force `history_learning_level = NO_HISTORY`. modore drives Mozc
    // as a one-shot "convert this span" tool, not as an interactive IME —
    // every successful convert lands a synthetic ENTER, which Mozc otherwise
    // treats as "user picked this candidate" and reinforces in
    // segment_history (segment.db / boundary.db). The katakana flow makes
    // this catastrophic: a single Shift+Ctrl+/ commits ニホンゴ, and from
    // then on plain Ctrl+/ on `nihongo` ranks ニホンゴ first — and each
    // subsequent commit reinforces it further. With NO_HISTORY, neither
    // user_history_predictor nor user_segment_history_rewriter writes,
    // and ranking stays consistent across sessions.
    {
        std::shared_ptr<const mozc::config::Config> cur =
            mozc::config::ConfigHandler::GetSharedConfig();
        mozc::config::Config seeded;
        if (!cur || cur->character_form_rules_size() == 0) {
            seeded = *mozc::config::ConfigHandler::GetSharedDefaultConfig();
        } else {
            seeded = *cur;
        }
        seeded.set_history_learning_level(mozc::config::Config::NO_HISTORY);
        // Disable the cascading "そのほかの文字種" submenu. With it on,
        // Mozc emits a single candidate whose value is literally that
        // label string — fine for a UI that renders submenus, but in
        // our flat cycle list it shows up as a junk entry the user
        // can't do anything with. Off → transliterations get inlined
        // into the main candidate list as ordinary entries.
        seeded.set_use_cascading_window(false);
        mozc::config::ConfigHandler::SetConfig(std::move(seeded));
    }

    g_initialized = true;
    clear_error();
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
    if (!g_initialized || !g_client) {
        set_error("mozc_bridge_init has not been called");
        return -1;
    }
    if (!romaji || !commit_len) {
        set_error("null pointer passed to mozc_bridge_convert");
        return -1;
    }
    // Zero the candidate-output fields up front so partial-failure paths
    // leave caller-visible state consistent (no garbage count if SPACE
    // failed and we never reached the capture step).
    if (cands_total_len) *cands_total_len = 0;
    if (out_candidate_count) *out_candidate_count = 0;
    if (romaji_len == 0) {
        if (commit_len) *commit_len = 0;
        clear_error();
        return 0;
    }

    auto *client = g_client.get();
    mozc::commands::Output out;

    // MozcDirectClient keeps one IME session for the process lifetime. Each convert feeds keys
    // into that session; without a reset, converter/history state from the previous call bleeds
    // into the next (e.g. "nihongo" committing the prior "test" candidate).
    {
        mozc::commands::SessionCommand reset;
        reset.set_type(mozc::commands::SessionCommand::RESET_CONTEXT);
        mozc::commands::Output reset_out;
        if (!client->SendCommandWithContext(
                reset, mozc::commands::Context::default_instance(), &reset_out)) {
            set_error("SendCommand RESET_CONTEXT failed (pre-convert)");
            return -1;
        }
    }

    // 1) Push each byte as an ASCII KeyEvent. Mozc's composer accumulates
    //    the romaji into a hiragana preedit. Non-ASCII spans are passed
    //    through one byte at a time; mozc will treat anything outside its
    //    romaji table as a passthrough character.
    for (size_t i = 0; i < romaji_len; ++i) {
        unsigned char c = static_cast<unsigned char>(romaji[i]);
        mozc::commands::KeyEvent k;
        k.set_key_code(c);
        if (!send_key(client, k, &out, "romaji")) {
          reset_session_best_effort(client);
          return -1;
        }
    }

    // 2) Conversion. Three sub-flows depending on flags and whether the
    //    caller asked for the candidate list.
    //
    //    a) Plain kanji (default): SPACE enters conversion state and
    //       auto-selects the top candidate. SPACE *also* flushes any
    //       half-formed romaji left in the composer — e.g. the trailing
    //       "n" of "henkan" becomes "ん" because Mozc treats it as
    //       word-final once SPACE enters conversion mode. ENTER commits.
    //
    //    b) Katakana (MOZC_CONVERT_FLAG_KATAKANA): SPACE + F7. F7 alone
    //       (what JIS keyboards send for "convert to katakana") only
    //       operates on what's already kana, so the trailing pending "n"
    //       of "henkan" would survive into the output as a full-width
    //       Latin "ｎ" (e.g. "henkan" → "ヘンカｎ"). SPACE first flushes
    //       the pending romaji into kana; F7 then swaps the segment's
    //       form to full-width katakana; ENTER commits.
    //
    //    c) Kanji *with candidates* (cands_buf != NULL, no katakana
    //       flag): two SPACEs. The first enters conversion state with
    //       the top candidate auto-selected, but Mozc does NOT populate
    //       `output.candidate_window` until the user explicitly opens
    //       the candidate window — which the second SPACE does (mapped
    //       to "ConvertNext" in the default keymap). After the second
    //       SPACE, focus has moved to candidate index 1; we capture the
    //       list, then use SessionCommand::SUBMIT_CANDIDATE with id=0
    //       to commit candidate 0 instead of the focused one. ENTER
    //       would commit whatever's focused (= candidate 1), which is
    //       not what the caller wants.
    const bool force_katakana = (flags & MOZC_CONVERT_FLAG_KATAKANA) != 0u;
    const bool capture_cands = (cands_buf != nullptr) && (cands_cap > 0) && !force_katakana;
    {
        mozc::commands::KeyEvent k;
        k.set_special_key(mozc::commands::KeyEvent::SPACE);
        if (!send_key(client, k, &out, "space")) {
          reset_session_best_effort(client);
          return -1;
        }
    }
    if (force_katakana) {
        mozc::commands::KeyEvent k;
        k.set_special_key(mozc::commands::KeyEvent::F7);
        if (!send_key(client, k, &out, "f7")) {
          reset_session_best_effort(client);
          return -1;
        }
    } else if (capture_cands) {
        // Open the candidate window so the full list is exposed.
        mozc::commands::KeyEvent k;
        k.set_special_key(mozc::commands::KeyEvent::SPACE);
        if (!send_key(client, k, &out, "space2")) {
          reset_session_best_effort(client);
          return -1;
        }
    }

    // 2.5) Capture top-N candidates *before* the commit step overwrites
    //      the output. `output.candidate_window` is populated once the
    //      candidate window has been opened (after the second SPACE in
    //      the kanji-with-candidates flow). Empty otherwise — we still
    //      check has_candidate_window() to stay safe on edge cases.
    //
    //      Truncation is silent — buffer-full just stops writing and
    //      returns the count actually emitted. NUL-separated UTF-8 keeps
    //      the C ABI simple: callers parse by walking the buffer.
    if (capture_cands && out.has_candidate_window()) {
        const auto &cw = out.candidate_window();
        const int limit = (max_candidates > 0)
            ? std::min(max_candidates, cw.candidate_size())
            : cw.candidate_size();
        size_t written = 0;
        int count = 0;
        for (int i = 0; i < limit; ++i) {
            const std::string &v = cw.candidate(i).value();
            const size_t needed = v.size() + 1;  // value + NUL separator
            if (written + needed > cands_cap) break;
            std::memcpy(cands_buf + written, v.data(), v.size());
            cands_buf[written + v.size()] = '\0';
            written += needed;
            ++count;
        }
        if (cands_total_len)     *cands_total_len     = written;
        if (out_candidate_count) *out_candidate_count = count;
    }

    // 3) Commit (or extract without committing for katakana).
    //
    //    Kanji + no-cands:        plain ENTER on the focused candidate.
    //    Kanji + capture-cands:   second SPACE moved focus to index 1, so
    //                             SUBMIT_CANDIDATE(id=0) to commit the top.
    //    Katakana:                read the post-F7 preedit and DO NOT
    //                             commit — every ENTER updates Mozc's
    //                             user_history, and committing a katakana
    //                             form for romaji like "nihongo" teaches
    //                             Mozc that the user prefers katakana for
    //                             that reading, so the *next* normal
    //                             conversion ranks katakana first. Skip
    //                             the commit and the history doesn't see
    //                             the katakana request at all.
    std::string katakana_preedit;
    if (force_katakana) {
        for (int i = 0; i < out.preedit().segment_size(); ++i) {
            katakana_preedit += out.preedit().segment(i).value();
        }
    } else if (capture_cands) {
        mozc::commands::SessionCommand cmd;
        cmd.set_type(mozc::commands::SessionCommand::SUBMIT_CANDIDATE);
        cmd.set_id(0);
        if (!client->SendCommandWithContext(
                cmd, mozc::commands::Context::default_instance(), &out)) {
            set_error("SubmitCandidate(0) failed");
            reset_session_best_effort(client);
            return -1;
        }
    } else {
        mozc::commands::KeyEvent k;
        k.set_special_key(mozc::commands::KeyEvent::ENTER);
        if (!send_key(client, k, &out, "enter")) {
          reset_session_best_effort(client);
          return -1;
        }
    }

    const std::string &committed =
        force_katakana ? katakana_preedit : out.result().value();
    if (committed.empty()) {
        // Mozc didn't produce a commit (very rare for a normal romaji string).
        // Best-effort: fall back to the preedit text if present, else the
        // original input. The caller will see *some* replacement rather than
        // a silent failure.
        const std::string preedit = [&] {
            std::string s;
            for (int i = 0; i < out.preedit().segment_size(); ++i) {
                s += out.preedit().segment(i).value();
            }
            return s;
        }();
        const std::string &fallback =
            !preedit.empty() ? preedit : std::string(romaji, romaji_len);
        if (fallback.size() > commit_cap) {
            clear_error();
            reset_session_best_effort(client);
            return static_cast<int>(fallback.size());
        }
        if (commit_buf) std::memcpy(commit_buf, fallback.data(), fallback.size());
        *commit_len = fallback.size();
        clear_error();
        reset_session_best_effort(client);
        return 0;
    }

    if (committed.size() > commit_cap) {
        clear_error();
        reset_session_best_effort(client);
        return static_cast<int>(committed.size());
    }
    if (commit_buf) std::memcpy(commit_buf, committed.data(), committed.size());
    *commit_len = committed.size();
    clear_error();

    reset_session_best_effort(client);
    return 0;
}

extern "C" void mozc_bridge_shutdown(void) {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    g_client.reset();
    g_initialized = false;
    clear_error();
}

extern "C" const char *mozc_bridge_last_error(void) {
    return g_last_error.empty() ? nullptr : g_last_error.c_str();
}
