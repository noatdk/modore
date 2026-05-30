/* modore_script — Lua scripting engine for modore (host-loaded shared lib).
 *
 * Public API prefix: mdr_*. ABI v1. Stable contract between this library
 * and every host (macOS, Linux, future Windows). All strings are UTF-8
 * with explicit byte lengths; offsets count bytes, never code points or
 * UTF-16 units. The macOS host owns the AX UTF-16 ↔ UTF-8 conversion at
 * its boundary.
 *
 * Trust model: scripts run with local-user trust. The sandbox strips
 * `io`, `os.execute`, `os.popen`, `package`, `require`, `ffi`, and
 * `debug`. It is a deterrent against footguns, not a security boundary
 * against malicious scripts — Lua bytecode can break out of any sandbox.
 *
 * Threading: a single mdr_engine_t* is owned by one thread. Hosts that
 * call from multiple threads must serialize externally.
 *
 * Per-hook opt-in is a hard invariant. Every hook (`on_pickup`,
 * `on_replacement`, `route_for_app`, `on_candidates`, `on_acquire`) is
 * independently optional. A script may define one, all, or none. Engine
 * behaviour for "missing hook", "hook returns nil", and "hook errors" is
 * identical: the mdr_* hook returns 0, telling the host to use its built-
 * in default.
 */

#ifndef MODORE_SCRIPT_H
#define MODORE_SCRIPT_H

#include <stddef.h>

/* Shared romaji/ASCII text policy (mdr_byte_bounds_t + mdr_text_*). Split out
 * so the bridge can compile the same implementation without the Lua ABI. */
#include "mdr_text.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef MDR_BUILDING
#    define MDR_EXPORT __declspec(dllexport)
#  else
#    define MDR_EXPORT __declspec(dllimport)
#  endif
#else
#  define MDR_EXPORT __attribute__((visibility("default")))
#endif

#define MDR_ABI_VERSION 1

typedef struct mdr_engine mdr_engine_t;

/* ----- Logging ---------------------------------------------------------- */

typedef enum {
    MDR_LOG_INFO  = 0,
    MDR_LOG_WARN  = 1,
    MDR_LOG_ERROR = 2
} mdr_log_level_t;

/* msg is NUL-terminated UTF-8 owned by the engine; callback must copy
 * if it needs to outlive the call. Return value is reserved (use 0). */
typedef int (*mdr_log_cb)(void* userdata, int level, const char* tag, const char* msg);

/* ----- Hook context payloads ------------------------------------------- */

typedef struct {
    const char* full_text;
    size_t      full_text_len;
    size_t      caret_byte;
    const char* app_id;            /* bundle id / wm-class; NULL if unknown */
    const char* field_role;        /* AX role / UI role; NULL if unknown */
    const char* field_description;  /* AX description; NULL if unknown */
    unsigned    flags;             /* bit 0 = katakana modifier held */
} mdr_pickup_ctx_t;

typedef struct {
    size_t      span_start_byte;
    size_t      span_end_byte;
    const char* romaji;
    size_t      romaji_len;
} mdr_span_t;

/* mdr_byte_bounds_t lives in mdr_text.h (included above). */

/* ----- Routing --------------------------------------------------------- */

typedef enum {
    MDR_ROUTE_DEFAULT   = 0,
    MDR_ROUTE_AX        = 1,
    MDR_ROUTE_SELECTION_SYNC = 2,
    MDR_ROUTE_KEYSTROKE = 3,
    MDR_ROUTE_CLIPBOARD = 4
} mdr_route_t;

/* ----- Host-default trampolines ---------------------------------------- */

typedef int (*mdr_default_pickup_fn)(
    void* host_ud,
    const mdr_pickup_ctx_t* ctx,
    mdr_span_t* out_span);

typedef int (*mdr_default_replacement_fn)(
    void* host_ud,
    const char* app_id,
    const mdr_span_t* span,
    const char* const* cands, size_t n_cands,
    char* out_buf, size_t out_cap, size_t* out_len);

typedef int (*mdr_default_route_fn)(
    void* host_ud,
    const char* app_id,
    mdr_route_t* out_route);

/* ----- Lifecycle ------------------------------------------------------- */

MDR_EXPORT mdr_engine_t* mdr_init(void);
MDR_EXPORT void          mdr_shutdown(mdr_engine_t*);
MDR_EXPORT int           mdr_abi_version(void);

MDR_EXPORT int mdr_set_log_callback(
    mdr_engine_t*, mdr_log_cb cb, void* userdata);

MDR_EXPORT int mdr_set_defaults(
    mdr_engine_t*, void* host_userdata,
    mdr_default_pickup_fn pickup_fn,
    mdr_default_replacement_fn replacement_fn,
    mdr_default_route_fn route_fn);

/* Load scripts from a directory. Recognised files:
 *   default.lua            — fallback script for any app
 *   <app_id>.lua           — per-app script (e.g. md.obsidian.lua)
 * Missing directory is not an error; engine just runs in pass-through.
 * Returns 0 on success, -1 on irrecoverable failure. Per-script load
 * failures are logged via the log callback; that script is skipped. */
MDR_EXPORT int mdr_load_dir(mdr_engine_t*, const char* dir_path);

/* ----- Hook invocation -------------------------------------------------
 *
 * Each mdr_* hook returns:
 *    1   script produced a result; out_* fields populated.
 *    0   no script result → host should use its default implementation.
 *        (No matching script, no such hook, hook returned nil, or hook
 *         raised an error. All four are indistinguishable to the host.)
 *   -1   engine-level failure (bad args, NULL handle, etc.).
 *
 * out_buf parameters are size-capped; engine writes at most out_cap bytes.
 * out_len receives the byte count written (excluding any NUL terminator).
 * NUL is appended when out_cap > out_len for caller convenience.
 */

MDR_EXPORT int mdr_pickup(
    mdr_engine_t*,
    const mdr_pickup_ctx_t* ctx,
    mdr_span_t* out_span);

MDR_EXPORT int mdr_replacement(
    mdr_engine_t*,
    const char* app_id,
    const mdr_span_t* span,
    const char* const* cands, size_t n_cands,
    char* out_buf, size_t out_cap, size_t* out_len);

/* route_for_app(ctx, api) can return "default", "ax", "selection_sync",
 * "keystroke", or "clipboard". The ctx table carries the same pickup
 * metadata as on_acquire. The host passes the script's `modore` table as
 * the explicit `api` argument so stage callbacks can invoke helpers
 * imperatively. */
MDR_EXPORT int mdr_route(
    mdr_engine_t*,
    const mdr_pickup_ctx_t* ctx,
    mdr_route_t* out_route);

/* in_cands[]: array of NUL-terminated UTF-8 candidates.
 * out_buf:    receives NUL-separated UTF-8 candidates (each followed by
 *             a single 0x00). out_count receives the number written. */
MDR_EXPORT int mdr_candidates(
    mdr_engine_t*,
    const char* app_id,
    const char* const* in_cands, size_t n_in,
    int current_idx,
    char* out_buf, size_t out_cap, size_t* out_count);

/* ----- Host primitives ------------------------------------------------- */

/* The on_acquire hook is imperative: scripts compose their own
 * text-acquisition routine by calling primitives the host registered.
 * Each primitive may be NULL — Lua-side `modore.host.<name>` returns nil
 * if the host didn't wire that primitive.
 *
 * Threading: every primitive is invoked from the host's pickup thread,
 * synchronously, while a script hook is on the stack. Hosts must call
 * any AppKit / Cocoa work via the relevant queue if needed (the macOS
 * implementation marshals to main thread for AX where required).
 */
typedef struct {
    /* Send a key chord, e.g. "shift+cmd+left", "cmd+c". Syntax is the
     * host's chord parser (the same one used for hotkey config). */
    void (*send_chord)(void* host_ud, const char* chord);

    /* Block for `ms` milliseconds. */
    void (*sleep_ms)(void* host_ud, unsigned ms);

    /* Read current clipboard text. Returns NUL-terminated UTF-8 written
     * into out_buf; NUL-terminator counted in out_cap. Returns 1 on
     * success, 0 if clipboard is empty / not text. */
    int  (*clipboard_read)(void* host_ud, char* out_buf, size_t out_cap, size_t* out_len);

    /* Write text to clipboard. Returns 1 on success. */
    int  (*clipboard_write)(void* host_ud, const char* text, size_t len);

    /* Read the focused-element's currently selected text via the host's
     * accessibility surface (no clipboard touch). Lets scripts pick up the
     * active selection without sending Cmd+C, which in some editors
     * (Obsidian/CodeMirror) silently extends the range linewise and
     * corrupts the replacement boundary. Returns NUL-terminated UTF-8 in
     * out_buf; NUL-terminator counted in out_cap. Returns 1 on success,
     * 0 if AX is unavailable / no element focused / nothing selected. */
    int  (*read_selection)(void* host_ud, char* out_buf, size_t out_cap, size_t* out_len);
} mdr_host_ops_t;

MDR_EXPORT int mdr_set_host_ops(mdr_engine_t*, const mdr_host_ops_t* ops, void* host_userdata);

/* Per-app text-acquisition hook.
 *
 * The host calls this at the top of its pickup pipeline. The script's
 * `on_acquire(ctx, api)` composes a routine using `modore.host.*`
 * primitives and the explicit `api` argument (the script's `modore`
 * table) and returns either:
 *
 *   - a string: the picked text. Host treats this as the equivalent of
 *     what `Cmd+C` would have produced — host strips trailing newlines,
 *     runs Mozc, and writes the replacement back. The script is expected
 *     to leave the focused-app selection ACTIVE on the picked text so
 *     the replacement injection lands on it.
 *
 *   - nil: no opinion; host uses its built-in acquisition path
 *     (AX read first, clipboard fallback).
 *
 * Same context shape as on_pickup (full_text + caret + app_id + flags).
 * On the imperative path the host typically passes app_id only; full_text
 * may be empty because the host hasn't read anything yet.
 */
MDR_EXPORT int mdr_acquire(
    mdr_engine_t*,
    const mdr_pickup_ctx_t* ctx,
    char* out_buf, size_t out_cap, size_t* out_len);

/* Portable pickup core (mdr_text_*): stateless UTF-8 span/segmentation policy
 * shared by native hosts, Lua, and the bridge — declared in mdr_text.h. */

#ifdef __cplusplus
}
#endif

#endif /* MODORE_SCRIPT_H */
