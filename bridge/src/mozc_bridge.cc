// modeless-ime — bridge implementation.
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

#include <cstring>
#include <mutex>
#include <string>

#include "absl/log/initialize.h"
#include "direct_client.h"
#include "protocol/commands.pb.h"

namespace {

thread_local std::string g_last_error;

std::mutex g_init_mutex;
bool g_initialized = false;
std::unique_ptr<modeless_ime::mozc_bridge::MozcDirectClient> g_client;

void set_error(const char *msg) { g_last_error.assign(msg ? msg : ""); }
void set_error(const std::string &msg) { g_last_error = msg; }
void clear_error() { g_last_error.clear(); }

// Send a single key event; returns false (and sets last_error) on failure.
bool send_key(modeless_ime::mozc_bridge::MozcDirectClient *client,
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

}  // namespace

extern "C" int mozc_bridge_init(const char *user_profile_dir) {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_initialized) {
        clear_error();
        return 0;
    }

    // mozc reads MOZC_USER_PROFILE_DIRECTORY from the environment when it
    // initializes its file storage. Setting it before constructing the client
    // is what wires our profile dir in.
    if (user_profile_dir && *user_profile_dir) {
        setenv("MOZC_USER_PROFILE_DIRECTORY", user_profile_dir, /*overwrite=*/1);
    }

    // Quiet absl's flag-parse log spam. Calls without flags init are fine
    // for embedded use.
    absl::InitializeLog();

    try {
        g_client =
            std::make_unique<modeless_ime::mozc_bridge::MozcDirectClient>();
    } catch (const std::exception &e) {
        set_error(std::string("MozcDirectClient ctor threw: ") + e.what());
        return -1;
    } catch (...) {
        set_error("MozcDirectClient ctor threw unknown exception");
        return -1;
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

    // 1) Push each byte as an ASCII KeyEvent. Mozc's composer accumulates
    //    the romaji into a hiragana preedit. Non-ASCII spans are passed
    //    through one byte at a time; mozc will treat anything outside its
    //    romaji table as a passthrough character.
    for (size_t i = 0; i < romaji_len; ++i) {
        unsigned char c = static_cast<unsigned char>(romaji[i]);
        mozc::commands::KeyEvent k;
        k.set_key_code(c);
        if (!send_key(client, k, &out, "romaji")) return -1;
    }

    // 2) SPACE → enter conversion mode (top candidate becomes preedit).
    {
        mozc::commands::KeyEvent k;
        k.set_special_key(mozc::commands::KeyEvent::SPACE);
        if (!send_key(client, k, &out, "space")) return -1;
    }

    // 3) ENTER → commit top candidate. result.value() holds the committed
    //    Japanese text.
    {
        mozc::commands::KeyEvent k;
        k.set_special_key(mozc::commands::KeyEvent::ENTER);
        if (!send_key(client, k, &out, "enter")) return -1;
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
            return static_cast<int>(fallback.size());
        }
        if (out_buf) std::memcpy(out_buf, fallback.data(), fallback.size());
        *out_len = fallback.size();
        clear_error();
        return 0;
    }

    if (committed.size() > out_cap) {
        clear_error();
        return static_cast<int>(committed.size());
    }
    if (out_buf) std::memcpy(out_buf, committed.data(), committed.size());
    *out_len = committed.size();
    clear_error();
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
