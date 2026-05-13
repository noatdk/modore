/* reload.c — script discovery, loading, and mtime-poll reload. */

#include "engine_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char* HOOK_FIELD[MS_HOOK_COUNT] = {
    "on_pickup",       /* MS_HOOK_PICKUP */
    "on_replacement",  /* MS_HOOK_REPLACEMENT */
    "route_for_app",   /* MS_HOOK_ROUTE */
    "on_candidates",   /* MS_HOOK_CANDIDATES */
    "on_acquire",      /* MS_HOOK_ACQUIRE */
};

/* Pick the script entry whose app_id matches; fall back to default
 * (app_id == NULL) entry. Returns NULL if no script loaded at all. */
script_entry_t* ms_pick_script(mdr_engine_t* eng, const char* app_id) {
    if (!eng || eng->n_scripts == 0) return NULL;
    script_entry_t* fallback = NULL;
    for (size_t i = 0; i < eng->n_scripts; ++i) {
        script_entry_t* s = &eng->scripts[i];
        if (!s->app_id) { fallback = s; continue; }
        if (app_id && strcmp(s->app_id, app_id) == 0) return s;
    }
    return fallback;
}

int ms_add_script(mdr_engine_t* eng, const char* app_id, const char* path) {
    if (!eng || !path) return -1;
    if (eng->n_scripts == eng->cap_scripts) {
        size_t newcap = eng->cap_scripts ? eng->cap_scripts * 2 : 4;
        script_entry_t* g = (script_entry_t*)realloc(eng->scripts, newcap * sizeof(*g));
        if (!g) return -1;
        eng->scripts     = g;
        eng->cap_scripts = newcap;
    }
    script_entry_t* s = &eng->scripts[eng->n_scripts++];
    memset(s, 0, sizeof(*s));
    s->app_id = app_id ? strdup(app_id) : NULL;
    s->path   = strdup(path);
    s->env_ref = LUA_NOREF;
    for (int h = 0; h < MS_HOOK_COUNT; ++h) s->hook_ref[h] = LUA_NOREF;
    s->any_hook = 0;
    s->mtime = 0;
    s->size  = 0;
    return 0;
}

/* Wipe the per-script Lua state (env + hook refs). */
void ms_clear_script(mdr_engine_t* eng, script_entry_t* s) {
    if (!eng || !s) return;
    lua_State* L = eng->L;
    for (int h = 0; h < MS_HOOK_COUNT; ++h) {
        if (s->hook_ref[h] != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, s->hook_ref[h]);
            s->hook_ref[h] = LUA_NOREF;
        }
    }
    if (s->env_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s->env_ref);
        s->env_ref = LUA_NOREF;
    }
    s->any_hook = 0;
}

/* Scan the env's modore table for the four hook functions, parking each
 * in the registry. */
static void cache_hooks(mdr_engine_t* eng, script_entry_t* s, int env_idx) {
    lua_State* L = eng->L;
    lua_getfield(L, env_idx, "modore");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    int modore_idx = lua_gettop(L);
    for (int h = 0; h < MS_HOOK_COUNT; ++h) {
        lua_getfield(L, modore_idx, HOOK_FIELD[h]);
        if (lua_isfunction(L, -1)) {
            s->hook_ref[h] = luaL_ref(L, LUA_REGISTRYINDEX);
            s->any_hook |= (1u << h);
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);  /* modore */
}

/* Read the file, compile via loadbuffer, set env via setfenv, pcall.
 * Returns 0 on success, -1 on failure (already logged). */
int ms_load_script_file(mdr_engine_t* eng, script_entry_t* s) {
    lua_State* L = eng->L;

    /* Drop any existing state for this script before reload. */
    ms_clear_script(eng, s);

    FILE* fp = fopen(s->path, "rb");
    if (!fp) {
        ms_log(eng, MDR_LOG_WARN, "engine", "open '%s' failed: %s",
               s->path, strerror(errno));
        return -1;
    }
    /* fstat BEFORE reading: if the file is replaced mid-read, the stored
     * mtime/size matches what we actually loaded, not the next file on disk. */
    struct stat st;
    if (fstat(fileno(fp), &st) != 0) { fclose(fp); return -1; }
    s->mtime = st.st_mtime;
    s->size  = st.st_size;
    long n = st.st_size;
    if (n < 0) { fclose(fp); return -1; }

    char* buf = (char*)malloc((size_t)n);
    if (!buf) { fclose(fp); return -1; }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf); fclose(fp);
        ms_log(eng, MDR_LOG_WARN, "engine", "short read on '%s'", s->path);
        return -1;
    }
    fclose(fp);

    /* Compile. */
    int rc = luaL_loadbuffer(L, buf, (size_t)n, s->path);
    free(buf);
    if (rc != 0) {
        const char* err = lua_tostring(L, -1);
        ms_log(eng, MDR_LOG_ERROR, "lua", "load '%s': %s",
               s->path, err ? err : "(unknown)");
        lua_pop(L, 1);
        return -1;
    }

    /* Build env and apply via setfenv. */
    s->env_ref = ms_build_env(eng);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s->env_ref);  /* env */
    /* stack: chunk, env */
    lua_setfenv(L, -2);                              /* chunk now has env */

    /* Execute. */
    rc = lua_pcall(L, 0, 0, 0);
    if (rc != 0) {
        const char* err = lua_tostring(L, -1);
        ms_log(eng, MDR_LOG_ERROR, "lua", "exec '%s': %s",
               s->path, err ? err : "(unknown)");
        lua_pop(L, 1);
        luaL_unref(L, LUA_REGISTRYINDEX, s->env_ref);
        s->env_ref = LUA_NOREF;
        return -1;
    }

    /* Cache hook refs by scanning env.modore. */
    lua_rawgeti(L, LUA_REGISTRYINDEX, s->env_ref);
    cache_hooks(eng, s, lua_gettop(L));
    lua_pop(L, 1);

    return 0;
}

/* If the script file's mtime or size changed since last load, reload.
 * Returns 0 on success or no-op; -1 if reload was attempted and failed
 * (caller should treat hooks as undefined). */
int ms_maybe_reload(mdr_engine_t* eng, script_entry_t* s) {
    if (!eng || !s) return -1;
    struct stat st;
    if (stat(s->path, &st) != 0) {
        /* File vanished. Clear hooks; engine returns to defaults. */
        if (s->env_ref != LUA_NOREF) {
            ms_clear_script(eng, s);
            ms_log(eng, MDR_LOG_INFO, "engine", "script '%s' gone, reverted", s->path);
        }
        return -1;
    }
    if (st.st_mtime == s->mtime && st.st_size == s->size) return 0;
    ms_log(eng, MDR_LOG_INFO, "engine", "reloading '%s'", s->path);
    return ms_load_script_file(eng, s);
}
