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
 * `on_replacement`, `route_for_app`, `on_candidates`) is independently
 * optional. A script may define one, all, or none. Engine behaviour
 * for "missing hook", "hook returns nil", and "hook errors" is identical:
 * the mdr_* hook returns 0, telling the host to use its built-in default.
 */

#ifndef MODORE_SCRIPT_H
#define MODORE_SCRIPT_H

#include <stddef.h>

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
    unsigned    flags;             /* bit 0 = katakana modifier held */
} mdr_pickup_ctx_t;

typedef struct {
    size_t      span_start_byte;
    size_t      span_end_byte;
    const char* romaji;
    size_t      romaji_len;
} mdr_span_t;

/* ----- Routing --------------------------------------------------------- */

typedef enum {
    MDR_ROUTE_DEFAULT   = 0,
    MDR_ROUTE_AX        = 1,
    MDR_ROUTE_KEYSTROKE = 2,
    MDR_ROUTE_CLIPBOARD = 3
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

MDR_EXPORT int mdr_route(
    mdr_engine_t*,
    const char* app_id,
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

#ifdef __cplusplus
}
#endif

#endif /* MODORE_SCRIPT_H */
