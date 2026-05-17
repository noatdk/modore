/* engine_stub.c — no-Lua fallback for libmodore_script.
 *
 * Hosts still link one shared library for classifier + text helpers, but
 * scripting hooks become pass-through no-ops when the build excludes LuaJIT.
 */

#include "modore_script.h"

#include <stdlib.h>

struct mdr_engine {
    mdr_log_cb log_cb;
    void*      log_ud;

    void*                      host_ud;
    mdr_default_pickup_fn      def_pickup;
    mdr_default_replacement_fn def_replacement;
    mdr_default_route_fn       def_route;

    mdr_host_ops_t host_ops;
    void*          host_ops_ud;

    int disabled_log_emitted;
};

static void maybe_log_disabled(mdr_engine_t* eng) {
    if (!eng || !eng->log_cb || eng->disabled_log_emitted) return;
    eng->disabled_log_emitted = 1;
    eng->log_cb(
        eng->log_ud,
        MDR_LOG_INFO,
        "engine",
        "Lua scripting disabled at build time; running classifier/text helpers only");
}

mdr_engine_t* mdr_init(void) {
    return (mdr_engine_t*)calloc(1, sizeof(mdr_engine_t));
}

void mdr_shutdown(mdr_engine_t* eng) {
    free(eng);
}

int mdr_abi_version(void) {
    return MDR_ABI_VERSION;
}

int mdr_set_log_callback(mdr_engine_t* eng, mdr_log_cb cb, void* ud) {
    if (!eng) return -1;
    eng->log_cb = cb;
    eng->log_ud = ud;
    return 0;
}

int mdr_set_defaults(mdr_engine_t* eng,
                     void* host_ud,
                     mdr_default_pickup_fn pickup_fn,
                     mdr_default_replacement_fn replacement_fn,
                     mdr_default_route_fn route_fn) {
    if (!eng) return -1;
    eng->host_ud = host_ud;
    eng->def_pickup = pickup_fn;
    eng->def_replacement = replacement_fn;
    eng->def_route = route_fn;
    return 0;
}

int mdr_set_host_ops(mdr_engine_t* eng, const mdr_host_ops_t* ops, void* host_ud) {
    if (!eng) return -1;
    if (ops) eng->host_ops = *ops;
    eng->host_ops_ud = host_ud;
    return 0;
}

int mdr_load_dir(mdr_engine_t* eng, const char* dir_path) {
    if (!eng || !dir_path) return -1;
    maybe_log_disabled(eng);
    return 0;
}

int mdr_pickup(mdr_engine_t* eng, const mdr_pickup_ctx_t* ctx, mdr_span_t* out_span) {
    (void)eng;
    (void)ctx;
    (void)out_span;
    return 0;
}

int mdr_replacement(mdr_engine_t* eng,
                    const char* app_id,
                    const mdr_span_t* span,
                    const char* const* cands, size_t n_cands,
                    char* out_buf, size_t out_cap, size_t* out_len) {
    (void)eng;
    (void)app_id;
    (void)span;
    (void)cands;
    (void)n_cands;
    (void)out_buf;
    (void)out_cap;
    (void)out_len;
    return 0;
}

int mdr_route(mdr_engine_t* eng, const mdr_pickup_ctx_t* ctx, mdr_route_t* out_route) {
    (void)eng;
    (void)ctx;
    (void)out_route;
    return 0;
}

int mdr_candidates(mdr_engine_t* eng,
                   const char* app_id,
                   const char* const* in_cands, size_t n_in,
                   int current_idx,
                   char* out_buf, size_t out_cap, size_t* out_count) {
    (void)eng;
    (void)app_id;
    (void)in_cands;
    (void)n_in;
    (void)current_idx;
    (void)out_buf;
    (void)out_cap;
    (void)out_count;
    return 0;
}

int mdr_acquire(mdr_engine_t* eng,
                const mdr_pickup_ctx_t* ctx,
                char* out_buf, size_t out_cap, size_t* out_len) {
    (void)eng;
    (void)ctx;
    (void)out_buf;
    (void)out_cap;
    (void)out_len;
    return 0;
}
