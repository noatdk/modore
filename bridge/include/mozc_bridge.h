// modore — flat C ABI around the Mozc engine.
//
// This header is what each frontend (macOS Swift host, future Windows/Linux
// hosts, future CLIs) consumes to drive Japanese conversion. The convert-loop
// abstraction (romaji → top-candidate kanji) lives here so every frontend
// gets it for free.
//
// The implementation links MozcDirectClient (in-process, no daemon, no IPC) —
// see bridge/src/direct_client.{cc,h}. Until the engine is wired up the
// implementation returns a placeholder so the host can be smoke-tested
// end-to-end against the FFI layout itself.

#ifndef MOZC_BRIDGE_H_
#define MOZC_BRIDGE_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the engine. Safe to call more than once; subsequent calls are
// no-ops if init already succeeded.
//
// user_profile_dir: absolute path that mozc will use for its on-disk state
//                   (user dictionary, history, config). NULL or "" uses the
//                   engine default. We will later wire this to a path
//                   bootstrapped from the user's existing GoogleJapaneseInput
//                   profile dir on first launch.
//
// Returns 0 on success, non-zero on error (see mozc_bridge_last_error()).
int mozc_bridge_init(const char *user_profile_dir);

// Convert a romaji span to its top-candidate Japanese conversion.
//
// romaji / romaji_len: UTF-8 input bytes; romaji_len is in bytes, not chars.
// out_buf / out_cap:   caller-provided output buffer. UTF-8, not NUL-padded.
// out_len:             on success, set to bytes written (excluding any NUL).
//
// Returns 0 on success.
// Returns -1 on error; check mozc_bridge_last_error().
// Returns the required buffer size (positive, > out_cap) if the buffer is
//   too small; out_buf and *out_len are unchanged in that case.
int mozc_bridge_convert(const char *romaji,
                        size_t romaji_len,
                        char *out_buf,
                        size_t out_cap,
                        size_t *out_len);

// Flag bits accepted by mozc_bridge_convert_ex.
//
// Combine with bitwise OR; pass 0 for the default (top-candidate kanji
// conversion — same behavior as mozc_bridge_convert).
//
// KATAKANA: instead of letting Mozc pick its top kanji candidate, transform
// the entire composition to full-width katakana and commit that. Useful for
// loanwords where Mozc's top-1 is a forced kanji ("シドッチ" vs "史奉行").
#define MOZC_CONVERT_FLAG_KATAKANA 0x1u

// Convert a romaji span with explicit conversion-shape flags.
//
// Semantics, parameters, and return contract are identical to
// mozc_bridge_convert. The only addition is `flags` — a bitfield of
// MOZC_CONVERT_FLAG_* values controlling the target form. `flags = 0` is
// exactly equivalent to mozc_bridge_convert.
int mozc_bridge_convert_ex(const char *romaji,
                           size_t romaji_len,
                           char *out_buf,
                           size_t out_cap,
                           size_t *out_len,
                           unsigned int flags);

// Releases the engine. Optional — process exit is fine too.
void mozc_bridge_shutdown(void);

// Thread-local last error string. NULL if no error has occurred on this
// thread since the last successful call. Lifetime: valid until the next
// bridge call on the same thread.
const char *mozc_bridge_last_error(void);

#ifdef __cplusplus
}
#endif

#endif  // MOZC_BRIDGE_H_
