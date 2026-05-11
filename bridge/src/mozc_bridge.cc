// modore — bridge implementation.
//
// Drives MozcDirectClient (in-process, no daemon) to convert a romaji span
// into its top-candidate Japanese form. Wire shape:
//
//   for each byte c in romaji:
//     SendKey(KeyEvent{key_code = c})       // build the preedit
//   SendKey(KeyEvent{special_key = SPACE})  // trigger conversion
//   SendKey(KeyEvent{special_key = ENTER})  // commit top candidate
//   -> output.result().value()
//
// This loop intentionally always commits the top candidate. Candidate
// cycling (Ctrl+/ again to advance) will be added later by exposing the
// candidate list through a separate C ABI call; for now we lock in the
// most-common path: "convert this romaji and replace it".

#include "mozc_bridge.h"

#include <errno.h>

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
    {
        std::shared_ptr<const mozc::config::Config> cur =
            mozc::config::ConfigHandler::GetSharedConfig();
        if (cur && cur->character_form_rules_size() == 0) {
            mozc::config::Config seeded =
                *mozc::config::ConfigHandler::GetSharedDefaultConfig();
            mozc::config::ConfigHandler::SetConfig(std::move(seeded));
        }
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
    if (!g_initialized || !g_client) {
        set_error("mozc_bridge_init has not been called");
        return -1;
    }
    if (!romaji || !out_len) {
        set_error("null pointer passed to mozc_bridge_convert");
        return -1;
    }
    if (romaji_len == 0) {
        if (out_len) *out_len = 0;
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

    // 2) SPACE → enter conversion mode (top candidate becomes preedit).
    {
        mozc::commands::KeyEvent k;
        k.set_special_key(mozc::commands::KeyEvent::SPACE);
        if (!send_key(client, k, &out, "space")) {
          reset_session_best_effort(client);
          return -1;
        }
    }

    // 3) ENTER → commit top candidate. result.value() holds the committed
    //    Japanese text.
    {
        mozc::commands::KeyEvent k;
        k.set_special_key(mozc::commands::KeyEvent::ENTER);
        if (!send_key(client, k, &out, "enter")) {
          reset_session_best_effort(client);
          return -1;
        }
    }

    const std::string &committed = out.result().value();
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
        if (fallback.size() > out_cap) {
            clear_error();
            reset_session_best_effort(client);
            return static_cast<int>(fallback.size());
        }
        if (out_buf) std::memcpy(out_buf, fallback.data(), fallback.size());
        *out_len = fallback.size();
        clear_error();
        reset_session_best_effort(client);
        return 0;
    }

    if (committed.size() > out_cap) {
        clear_error();
        reset_session_best_effort(client);
        return static_cast<int>(committed.size());
    }
    if (out_buf) std::memcpy(out_buf, committed.data(), committed.size());
    *out_len = committed.size();
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
