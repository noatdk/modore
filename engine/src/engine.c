/* engine.c — lifecycle + public hook entry points. */

#include "engine_internal.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MS_TAG_ENGINE "engine"
#define MS_TAG_LUA    "lua"

/* ----- internal logging ------------------------------------------------ */

void ms_log(mdr_engine_t* eng, int level, const char* tag, const char* fmt, ...) {
    if (!eng || !eng->log_cb) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    eng->log_cb(eng->log_ud, level, tag ? tag : MS_TAG_ENGINE, buf);
}

/* ----- lifecycle ------------------------------------------------------- */

mdr_engine_t* mdr_init(void) {
    mdr_engine_t* eng = (mdr_engine_t*)calloc(1, sizeof(*eng));
    if (!eng) return NULL;

    eng->L = luaL_newstate();
    if (!eng->L) { free(eng); return NULL; }

    luaL_openlibs(eng->L);
    ms_register_modore_globals(eng);
    return eng;
}

void mdr_shutdown(mdr_engine_t* eng) {
    if (!eng) return;
    if (eng->scripts) {
        for (size_t i = 0; i < eng->n_scripts; ++i) {
            ms_clear_script(eng, &eng->scripts[i]);
            free(eng->scripts[i].app_id);
            free(eng->scripts[i].path);
        }
        free(eng->scripts);
    }
    free(eng->dir_path);
    if (eng->L) lua_close(eng->L);
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
    eng->host_ud         = host_ud;
    eng->def_pickup      = pickup_fn;
    eng->def_replacement = replacement_fn;
    eng->def_route       = route_fn;
    return 0;
}

/* ----- script dir loading --------------------------------------------- */

static int has_suffix(const char* s, const char* sfx) {
    size_t ls = strlen(s), lf = strlen(sfx);
    return ls >= lf && memcmp(s + ls - lf, sfx, lf) == 0;
}

int mdr_load_dir(mdr_engine_t* eng, const char* dir_path) {
    if (!eng || !dir_path) return -1;

    /* Replace dir_path; clear existing scripts. */
    for (size_t i = 0; i < eng->n_scripts; ++i) {
        ms_clear_script(eng, &eng->scripts[i]);
        free(eng->scripts[i].app_id);
        free(eng->scripts[i].path);
    }
    eng->n_scripts = 0;
    free(eng->dir_path);
    eng->dir_path = strdup(dir_path);

    DIR* d = opendir(dir_path);
    if (!d) {
        ms_log(eng, MDR_LOG_INFO, MS_TAG_ENGINE,
               "scripts dir '%s' missing — running in pass-through", dir_path);
        return 0;
    }

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        const char* name = ent->d_name;
        if (name[0] == '.') continue;
        if (!has_suffix(name, ".lua")) continue;

        size_t name_len = strlen(name);
        size_t dir_len  = strlen(dir_path);
        char* fullpath = (char*)malloc(dir_len + 1 + name_len + 1);
        if (!fullpath) continue;
        snprintf(fullpath, dir_len + 1 + name_len + 1, "%s/%s", dir_path, name);

        char* app_id = NULL;
        if (strcmp(name, "default.lua") != 0) {
            app_id = (char*)malloc(name_len - 4 + 1);
            if (app_id) memcpy(app_id, name, name_len - 4);
            if (app_id) app_id[name_len - 4] = '\0';
        }

        ms_add_script(eng, app_id, fullpath);
        free(app_id);    /* ms_add_script strdup'd them */
        free(fullpath);
    }
    closedir(d);

    /* Initial load. */
    for (size_t i = 0; i < eng->n_scripts; ++i) {
        ms_load_script_file(eng, &eng->scripts[i]);
    }
    return 0;
}

/* ----- hook invocation ------------------------------------------------- */

/* Push the script's hook fn onto L's stack. Returns 1 if pushed, 0 if
 * undefined / no such script / fast-path miss. */
static int push_hook(mdr_engine_t* eng, const char* app_id, int hook_id, script_entry_t** s_out) {
    script_entry_t* s = ms_pick_script(eng, app_id);
    if (!s) return 0;
    if (ms_maybe_reload(eng, s) != 0) return 0;
    if (!(s->any_hook & (1u << hook_id))) return 0;
    if (s->hook_ref[hook_id] == LUA_NOREF) return 0;
    lua_rawgeti(eng->L, LUA_REGISTRYINDEX, s->hook_ref[hook_id]);
    if (!lua_isfunction(eng->L, -1)) { lua_pop(eng->L, 1); return 0; }
    if (s_out) *s_out = s;
    return 1;
}

/* Run the function on top of stack with nargs args, expecting 1 result.
 * On success, leaves result on stack and returns 1. On error or nil result,
 * pops everything and returns 0. */
static int pcall_one(mdr_engine_t* eng, int nargs) {
    int rc = lua_pcall(eng->L, nargs, 1, 0);
    if (rc != 0) {
        const char* err = lua_tostring(eng->L, -1);
        ms_log(eng, MDR_LOG_ERROR, MS_TAG_LUA, "%s", err ? err : "(unknown error)");
        lua_pop(eng->L, 1);
        return 0;
    }
    if (lua_isnil(eng->L, -1)) {
        lua_pop(eng->L, 1);
        return 0;
    }
    return 1;
}

int mdr_pickup(mdr_engine_t* eng,
                                const mdr_pickup_ctx_t* ctx,
                                mdr_span_t* out_span) {
    if (!eng || !ctx || !out_span) return -1;
    script_entry_t* s = NULL;
    if (!push_hook(eng, ctx->app_id, MS_HOOK_PICKUP, &s)) return 0;
    ms_push_pickup_ctx(eng->L, ctx);
    if (!pcall_one(eng, 1)) return 0;
    /* Caller's storage for romaji is the romaji field of the inbound ctx
     * if scripts want to keep it; otherwise NULL. We don't allocate. */
    int ok = ms_pull_span(eng->L, -1, out_span, NULL, 0);
    lua_pop(eng->L, 1);
    return ok ? 1 : 0;
}

int mdr_replacement(mdr_engine_t* eng,
                                     const char* app_id,
                                     const mdr_span_t* span,
                                     const char* const* cands, size_t n_cands,
                                     char* out_buf, size_t out_cap, size_t* out_len) {
    if (!eng || !span || !out_buf || !out_len || out_cap == 0) return -1;
    script_entry_t* s = NULL;
    if (!push_hook(eng, app_id, MS_HOOK_REPLACEMENT, &s)) return 0;
    ms_push_span(eng->L, span);
    ms_push_cands(eng->L, cands, n_cands);
    if (!pcall_one(eng, 2)) return 0;
    if (lua_type(eng->L, -1) != LUA_TSTRING) {
        ms_log(eng, MDR_LOG_WARN, MS_TAG_LUA, "on_replacement returned non-string");
        lua_pop(eng->L, 1);
        return 0;
    }
    size_t len = 0;
    const char* str = lua_tolstring(eng->L, -1, &len);
    if (len >= out_cap) len = out_cap - 1;  /* clamp; reserve NUL */
    memcpy(out_buf, str, len);
    out_buf[len] = '\0';
    *out_len = len;
    lua_pop(eng->L, 1);
    return 1;
}

int mdr_route(mdr_engine_t* eng,
                                       const char* app_id,
                                       mdr_route_t* out_route) {
    if (!eng || !out_route) return -1;
    script_entry_t* s = NULL;
    if (!push_hook(eng, app_id, MS_HOOK_ROUTE, &s)) return 0;
    lua_pushstring(eng->L, app_id ? app_id : "");
    if (!pcall_one(eng, 1)) return 0;
    int ok = ms_pull_route(eng->L, -1, out_route);
    lua_pop(eng->L, 1);
    return ok ? 1 : 0;
}

int mdr_candidates(mdr_engine_t* eng,
                                    const char* app_id,
                                    const char* const* in_cands, size_t n_in,
                                    int current_idx,
                                    char* out_buf, size_t out_cap, size_t* out_count) {
    if (!eng || !out_buf || !out_count || out_cap == 0) return -1;
    script_entry_t* s = NULL;
    if (!push_hook(eng, app_id, MS_HOOK_CANDIDATES, &s)) return 0;
    ms_push_cands(eng->L, in_cands, n_in);
    lua_pushinteger(eng->L, current_idx);
    if (!pcall_one(eng, 2)) return 0;
    if (lua_type(eng->L, -1) != LUA_TTABLE) {
        ms_log(eng, MDR_LOG_WARN, MS_TAG_LUA, "on_candidates returned non-table");
        lua_pop(eng->L, 1);
        return 0;
    }
    size_t cnt = 0;
    size_t pos = 0;
    int n = (int)lua_objlen(eng->L, -1);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(eng->L, -1, i);
        if (lua_type(eng->L, -1) != LUA_TSTRING) { lua_pop(eng->L, 1); continue; }
        size_t slen = 0;
        const char* sval = lua_tolstring(eng->L, -1, &slen);
        if (pos + slen + 1 > out_cap) { lua_pop(eng->L, 1); break; }
        memcpy(out_buf + pos, sval, slen);
        out_buf[pos + slen] = '\0';
        pos += slen + 1;
        cnt++;
        lua_pop(eng->L, 1);
    }
    lua_pop(eng->L, 1);
    if (cnt == 0) {
        /* Empty list = treat as "use default", per per-hook opt-in spirit:
         * a script that doesn't want to override should just not write the
         * hook or return nil. Returning {} is taken as "no opinion". */
        return 0;
    }
    *out_count = cnt;
    return 1;
}
