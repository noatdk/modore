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

// Logical grouping for candidate presentation. This is a bridge-level hint
// derived from Mozc's candidate window category plus lightweight script
// classification; frontends may ignore it, but it is stable enough to drive
// section labels and muted transliteration rows.
typedef enum mozc_bridge_candidate_group {
  MOZC_CANDIDATE_GROUP_UNKNOWN = 0,
  MOZC_CANDIDATE_GROUP_CONVERSION = 1,
  MOZC_CANDIDATE_GROUP_TRANSLITERATION = 2,
  MOZC_CANDIDATE_GROUP_ENGLISH = 3,
  MOZC_CANDIDATE_GROUP_HIRAGANA = 4,
  MOZC_CANDIDATE_GROUP_KATAKANA = 5,
  MOZC_CANDIDATE_GROUP_INPUT = 6
} mozc_bridge_candidate_group_t;

// Metadata for one candidate entry. String fields are stored in a shared
// UTF-8 blob returned by mozc_bridge_convert_with_candidate_details_ex();
// each offset/length pair points into that blob. Missing strings use
// length=0 and offset=0.
typedef struct mozc_bridge_candidate_record {
  size_t value_offset;
  size_t value_len;
  size_t description_offset;
  size_t description_len;
  size_t prefix_offset;
  size_t prefix_len;
  size_t suffix_offset;
  size_t suffix_len;
  int id;
  unsigned int window_category;
  unsigned int group;
} mozc_bridge_candidate_record_t;

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

// Convert *and* return Mozc's top-N alternative candidates. Same commit
// behavior as mozc_bridge_convert_ex (top-1 is committed and copied into
// out_buf), with the added side-channel of the candidate list captured
// from Mozc's response between SPACE and ENTER. Used by frontends that
// want to offer candidate cycling without paying the cost of a second
// conversion round-trip per cycle press.
//
// cands_buf:       caller-provided output buffer for candidate strings.
//                  UTF-8, NUL-separated. Each candidate is followed by a
//                  single '\0' byte; no leading/trailing NUL beyond
//                  *cands_total_len.
// cands_cap:       capacity of cands_buf in bytes. Pass 0 / NULL buffer
//                  to skip candidate capture entirely (function then
//                  behaves like mozc_bridge_convert_ex).
// cands_total_len: on success, total bytes written into cands_buf
//                  (sum of candidate UTF-8 lengths + NUL separators).
// max_candidates:  cap on how many candidates to emit. <= 0 means "no
//                  cap, write as many as fit." Truncation is silent —
//                  if the buffer fills before max_candidates is reached,
//                  the partial list is returned without error.
// out_candidate_count: on success, number of candidates actually written.
// flags:           same MOZC_CONVERT_FLAG_* bits as convert_ex.
//
// Returns 0 on success. Same error contract as mozc_bridge_convert_ex for
// the commit half: -1 on error, positive value > out_cap if commit_buf is
// too small (candidate output is undefined when commit-side errors).
int mozc_bridge_convert_with_candidates_ex(const char *romaji,
                                           size_t romaji_len,
                                           char *commit_buf,
                                           size_t commit_cap,
                                           size_t *commit_len,
                                           char *cands_buf,
                                           size_t cands_cap,
                                           size_t *cands_total_len,
                                           int max_candidates,
                                           int *out_candidate_count,
                                           unsigned int flags);

// Convert and return structured candidate metadata. Same conversion behavior
// as mozc_bridge_convert_with_candidates_ex, but candidate strings and their
// annotations are serialized into `cand_strings_buf` and described by
// `cand_records`.
//
// cand_records:      caller-provided array of candidate records.
// cand_records_cap:  number of records available in cand_records.
// cand_strings_buf:  caller-provided UTF-8 blob backing all string fields in
//                    the records.
// cand_strings_cap:  capacity of cand_strings_buf in bytes.
// cand_strings_len:  on success, bytes written into cand_strings_buf.
//
// Truncation is silent: if either the record array or string blob fills,
// remaining candidates are skipped. `max_candidates <= 0` means "no explicit
// cap beyond the provided buffers."
int mozc_bridge_convert_with_candidate_details_ex(
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
    unsigned int flags);

// Convert a shell-editing line in place: identify the current token around
// `caret_byte`, convert that token with Mozc, and splice the result back into
// the original line. Shell frontends can call this from zsh/bash/fish widgets
// and let the shell itself rewrite its current buffer.
//
// Returns the same contract as the other convert helpers: 0 on success,
// -1 on error, positive > out_cap if the output buffer was too small.
int mozc_bridge_convert_line(const char *text,
                             size_t text_len,
                             size_t caret_byte,
                             char *out_buf,
                             size_t out_cap,
                             size_t *out_len,
                             unsigned int flags);

// Shell-native live-host transport.
//
// `mozc_bridge_shell_server_start` starts the resident socket listener used by
// shell bindings to reach the already-running modore host.
int mozc_bridge_shell_server_start(const char *socket_path);
void mozc_bridge_shell_server_stop(void);

// Client-side request helper for the shell binding. Sends the current line and
// caret position to the live host server above and returns the converted line.
int mozc_bridge_shell_convert_remote(const char *socket_path,
                                     const char *session_id_in,
                                     const char *mode_in,
                                     const char *text,
                                     size_t text_len,
                                     size_t caret_byte,
                                     char *out_buf,
                                     size_t out_cap,
                                     size_t *out_len);

// Return the current shell candidate list for a live session. The response is
// `session_id\ncurrent_index\ncandidate1\ncandidate2...`.
int mozc_bridge_shell_candidates_remote(const char *socket_path,
                                        const char *session_id_in,
                                        const char *mode_in,
                                        const char *text,
                                        size_t text_len,
                                        size_t caret_byte,
                                        char *out_buf,
                                        size_t out_cap,
                                        size_t *out_len);

// Commit a shell candidate selected by index. The response is
// `session_id\ncommitted_text`.
int mozc_bridge_shell_select_remote(const char *socket_path,
                                    const char *session_id_in,
                                    const char *mode_in,
                                    size_t selected_index,
                                    const char *text,
                                    size_t text_len,
                                    size_t caret_byte,
                                    char *out_buf,
                                    size_t out_cap,
                                    size_t *out_len);

// Print a shell bootstrap script for the current interactive shell.
// `hotkey_display_name` is the human-readable chord from modore.conf
// (for example "Ctrl+Shift+grave"). `host_executable_path` should point to
// the current modore-host binary (usually Bundle.main.executablePath on macOS).
// The bridge detects bash/zsh/fish from the process environment and emits a
// matching sourced snippet that binds the configured shell shortcut to that
// executable's `--shell-convert` subcommand.
int mozc_bridge_shell_bootstrap(const char *hotkey_display_name,
                                const char *host_executable_path,
                                char *out_buf,
                                size_t out_cap,
                                size_t *out_len);

// Releases the engine. Optional — process exit is fine too.
void mozc_bridge_shutdown(void);

// Internal bridge error helpers used by transport adapters.
void mozc_bridge_set_error(const char *msg);
void mozc_bridge_clear_error(void);

// Thread-local last error string. NULL if no error has occurred on this
// thread since the last successful call. Lifetime: valid until the next
// bridge call on the same thread.
const char *mozc_bridge_last_error(void);

#ifdef __cplusplus
}
#endif

#endif  // MOZC_BRIDGE_H_
