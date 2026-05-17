/* engine_internal.h — types shared across engine.c / sandbox.c /
 * marshal.c / reload.c. Not exported. */

#ifndef MODORE_SCRIPT_ENGINE_INTERNAL_H
#define MODORE_SCRIPT_ENGINE_INTERNAL_H

#include <sys/types.h>
#include <time.h>
#include <pthread.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "modore_script.h"

/* Hook slots. Order matches MS_HOOK_NAMES in sandbox.c. */
enum {
    MS_HOOK_PICKUP       = 0,
    MS_HOOK_REPLACEMENT  = 1,
    MS_HOOK_ROUTE        = 2,
    MS_HOOK_CANDIDATES   = 3,
    MS_HOOK_ACQUIRE      = 4,
    MS_HOOK_COUNT        = 5
};

typedef struct script_entry {
    char*           app_id;                /* NULL = default.lua */
    char*           path;                  /* absolute path */
    time_t          mtime;                 /* st_mtime when last loaded */
    off_t           size;                  /* st_size when last loaded */
    int             env_ref;               /* LUA_REGISTRYINDEX ref or LUA_NOREF */
    int             hook_ref[MS_HOOK_COUNT];
    int             any_hook;              /* bitset of hooks present */
} script_entry_t;

struct mdr_engine {
    lua_State*  L;
    pthread_mutex_t lock;

    mdr_log_cb log_cb;
    void*         log_ud;

    void*                          host_ud;
    mdr_default_pickup_fn       def_pickup;
    mdr_default_replacement_fn  def_replacement;
    mdr_default_route_fn        def_route;

    /* Host primitives for the on_acquire imperative path. NULL fn ptrs are
     * surfaced as nil to the Lua side. host_ops_ud may differ from host_ud
     * but in practice hosts use the same userdata for both. */
    mdr_host_ops_t host_ops;
    void*          host_ops_ud;

    script_entry_t* scripts;
    size_t          n_scripts;
    size_t          cap_scripts;

    char* dir_path;                        /* owned; NULL until load_dir */
};

/* Logging. ms_log copies into a stack buffer; truncates beyond ~512B. */
void ms_log(mdr_engine_t* eng, int level, const char* tag, const char* fmt, ...);

/* sandbox.c */
int  ms_build_env(mdr_engine_t* eng);   /* returns env_ref (LUA_REGISTRYINDEX) */
void ms_register_modore_globals(mdr_engine_t* eng); /* one-time at init */

/* marshal.c */
void ms_push_pickup_ctx(lua_State* L, const mdr_pickup_ctx_t* ctx);
void ms_push_span(lua_State* L, const mdr_span_t* span);
void ms_push_cands(lua_State* L, const char* const* cands, size_t n);
int  ms_pull_span(lua_State* L, int idx, mdr_span_t* out, char* romaji_buf, size_t romaji_cap);
int  ms_pull_route(lua_State* L, int idx, mdr_route_t* out);

/* reload.c */
script_entry_t* ms_pick_script(mdr_engine_t* eng, const char* app_id);
int  ms_maybe_reload(mdr_engine_t* eng, script_entry_t* s);
int  ms_load_script_file(mdr_engine_t* eng, script_entry_t* s);
void ms_clear_script(mdr_engine_t* eng, script_entry_t* s);
int  ms_add_script(mdr_engine_t* eng, const char* app_id, const char* path);

#endif
