/* sandbox.c — per-script env tables + the shared `modore` Lua library.
 *
 * Sandbox model: each script gets its own env table seeded with a
 * whitelist of stdlib functions and a per-script `modore` table. The
 * per-script `modore` table inherits log/default closures from a shared
 * base built once at engine init, but its hook slots (on_pickup, etc.)
 * are local — assignments stay isolated per script.
 *
 * Threat model: local-user trust. Bytecode-level escapes from a Lua 5.1
 * sandbox are well-known; we strip the obvious footguns (io, os.execute,
 * package, require, ffi, debug, load*) and call it good.
 */

#include "engine_internal.h"

#include <stdlib.h>
#include <string.h>

/* Registry keys for the shared library tables created once at init. */
static const char* RK_MODORE_LOG     = "modore.log.shared";
static const char* RK_MODORE_DEFAULT = "modore.default.shared";
static const char* RK_MODORE_HOST    = "modore.host.shared";
static const char* RK_MODORE_TEXT    = "modore.text.shared";

/* engine* pointer is stashed in registry under this key so C closures
 * can reach the engine without an upvalue per closure (cheaper, simpler). */
static const char* RK_ENGINE_PTR = "modore.engine.ptr";

/* ---- C-function helpers ---------------------------------------------- */

static mdr_engine_t* get_engine(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, RK_ENGINE_PTR);
    mdr_engine_t* eng = (mdr_engine_t*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return eng;
}

static int log_at(lua_State* L, int level) {
    mdr_engine_t* eng = get_engine(L);
    if (!eng) return 0;
    const char* msg = luaL_optstring(L, 1, "");
    ms_log(eng, level, "lua", "%s", msg);
    return 0;
}
static int log_info (lua_State* L) { return log_at(L, MDR_LOG_INFO);  }
static int log_warn (lua_State* L) { return log_at(L, MDR_LOG_WARN);  }
static int log_error(lua_State* L) { return log_at(L, MDR_LOG_ERROR); }

/* print() override — route to log.info. */
static int sandbox_print(lua_State* L) {
    mdr_engine_t* eng = get_engine(L);
    if (!eng) return 0;
    int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) luaL_addchar(&b, '\t');
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        size_t l;
        const char* s = lua_tolstring(L, -1, &l);
        if (s) luaL_addlstring(&b, s, l);
        lua_pop(L, 1);
    }
    luaL_pushresult(&b);
    const char* msg = lua_tostring(L, -1);
    ms_log(eng, MDR_LOG_INFO, "lua", "%s", msg ? msg : "");
    lua_pop(L, 1);
    return 0;
}

/* ---- modore.default.* trampolines ------------------------------------ */

static int default_pickup(lua_State* L) {
    mdr_engine_t* eng = get_engine(L);
    if (!eng || !eng->def_pickup) { lua_pushnil(L); return 1; }
    if (!lua_istable(L, 1))       { lua_pushnil(L); return 1; }

    mdr_pickup_ctx_t ctx = {0};
    lua_getfield(L, 1, "full_text");
    if (lua_type(L, -1) == LUA_TSTRING)
        ctx.full_text = lua_tolstring(L, -1, &ctx.full_text_len);
    lua_getfield(L, 1, "caret_byte");
    if (lua_isnumber(L, -1)) ctx.caret_byte = (size_t)lua_tointeger(L, -1);
    lua_getfield(L, 1, "app_id");
    if (lua_type(L, -1) == LUA_TSTRING) ctx.app_id = lua_tostring(L, -1);
    lua_getfield(L, 1, "flags");
    if (lua_isnumber(L, -1)) ctx.flags = (unsigned)lua_tointeger(L, -1);

    mdr_span_t out = {0};
    int rc = eng->def_pickup(eng->host_ud, &ctx, &out);
    lua_pop(L, 4);

    if (rc != 1) { lua_pushnil(L); return 1; }
    ms_push_span(L, &out);
    return 1;
}

static int default_replacement(lua_State* L) {
    mdr_engine_t* eng = get_engine(L);
    if (!eng || !eng->def_replacement) { lua_pushnil(L); return 1; }
    /* Args: span, cands_table. We're loose with arg validation — defaults
     * are called rarely; cost of being permissive is negligible. */
    if (!lua_istable(L, 1) || !lua_istable(L, 2)) { lua_pushnil(L); return 1; }

    mdr_span_t span = {0};
    if (!ms_pull_span(L, 1, &span, NULL, 0)) { lua_pushnil(L); return 1; }

    /* Collect candidates from the table. */
    int n = (int)lua_objlen(L, 2);
    const char** cands = (const char**)calloc((size_t)(n > 0 ? n : 1), sizeof(*cands));
    if (!cands) { lua_pushnil(L); return 1; }
    int real_n = 0;
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        if (lua_type(L, -1) == LUA_TSTRING) {
            cands[real_n++] = lua_tostring(L, -1);
            /* leave on stack so the C-string stays alive until call */
        } else {
            lua_pop(L, 1);
        }
    }

    /* app_id is not in the Lua call surface for default.replacement; if a
     * script needs to route per-app it should branch in its hook before
     * calling default. We pass NULL here. */
    char outbuf[512];
    size_t outlen = 0;
    int rc = eng->def_replacement(eng->host_ud, NULL, &span,
                                  cands, (size_t)real_n,
                                  outbuf, sizeof(outbuf), &outlen);
    /* Pop the candidates we left on the stack. */
    lua_pop(L, real_n);
    free(cands);

    if (rc != 1) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, outbuf, outlen);
    return 1;
}

static int default_route(lua_State* L) {
    mdr_engine_t* eng = get_engine(L);
    if (!eng || !eng->def_route) { lua_pushnil(L); return 1; }
    const char* app_id = NULL;
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "app_id");
        if (lua_type(L, -1) == LUA_TSTRING) app_id = lua_tostring(L, -1);
        lua_pop(L, 1);
    } else {
        app_id = luaL_optstring(L, 1, NULL);
    }
    mdr_route_t r = MDR_ROUTE_DEFAULT;
    int rc = eng->def_route(eng->host_ud, app_id, &r);
    if (rc != 1) { lua_pushnil(L); return 1; }
    switch (r) {
        case MDR_ROUTE_AX:        lua_pushliteral(L, "ax");        break;
        case MDR_ROUTE_SELECTION_SYNC: lua_pushliteral(L, "selection_sync"); break;
        case MDR_ROUTE_KEYSTROKE: lua_pushliteral(L, "keystroke"); break;
        case MDR_ROUTE_CLIPBOARD: lua_pushliteral(L, "clipboard"); break;
        default:                     lua_pushliteral(L, "default");   break;
    }
    return 1;
}

static int default_candidates(lua_State* L) {
    if (!lua_istable(L, 1)) { lua_pushnil(L); return 1; }
    lua_pushvalue(L, 1);
    return 1;
}

static int default_acquire(lua_State* L) {
    mdr_engine_t* eng = get_engine(L);
    if (!eng) { lua_pushnil(L); return 1; }

    if (eng->host_ops.read_selection) {
        char sel[4096];
        size_t sel_len = 0;
        if (eng->host_ops.read_selection(eng->host_ops_ud, sel, sizeof(sel), &sel_len) == 1 &&
            sel_len > 0) {
            lua_pushlstring(L, sel, sel_len);
            return 1;
        }
    }

    if (!eng->host_ops.send_chord ||
        !eng->host_ops.sleep_ms ||
        !eng->host_ops.clipboard_read ||
        !eng->host_ops.clipboard_write) {
        lua_pushnil(L);
        return 1;
    }

    char saved[4096];
    size_t saved_len = 0;
    int have_saved = eng->host_ops.clipboard_read(eng->host_ops_ud, saved, sizeof(saved), &saved_len) == 1;
    if (have_saved && saved_len >= sizeof(saved)) saved_len = sizeof(saved) - 1;
    if (have_saved) saved[saved_len] = '\0';

    eng->host_ops.send_chord(eng->host_ops_ud, "shift+alt+left");
    eng->host_ops.sleep_ms(eng->host_ops_ud, 20);
    eng->host_ops.send_chord(eng->host_ops_ud, "cmd+c");
    eng->host_ops.sleep_ms(eng->host_ops_ud, 30);

    char picked[4096];
    size_t picked_len = 0;
    int rc = eng->host_ops.clipboard_read(eng->host_ops_ud, picked, sizeof(picked), &picked_len);

    if (have_saved) {
        eng->host_ops.clipboard_write(eng->host_ops_ud, saved, saved_len);
    }

    if (rc != 1 || picked_len == 0) {
        lua_pushnil(L);
        return 1;
    }

    if (picked_len >= sizeof(picked)) picked_len = sizeof(picked) - 1;
    lua_pushlstring(L, picked, picked_len);
    return 1;
}

/* ---- modore.host.* imperative primitives ----------------------------- */

static int host_send_chord(lua_State* L) {
    mdr_engine_t* eng = get_engine(L);
    if (!eng || !eng->host_ops.send_chord) return 0;
    const char* chord = luaL_optstring(L, 1, "");
    eng->host_ops.send_chord(eng->host_ops_ud, chord);
    return 0;
}

static int host_sleep_ms(lua_State* L) {
    mdr_engine_t* eng = get_engine(L);
    if (!eng || !eng->host_ops.sleep_ms) return 0;
    lua_Integer ms = luaL_optinteger(L, 1, 0);
    if (ms < 0) ms = 0;
    eng->host_ops.sleep_ms(eng->host_ops_ud, (unsigned)ms);
    return 0;
}

static int host_clipboard_read(lua_State* L) {
    mdr_engine_t* eng = get_engine(L);
    if (!eng || !eng->host_ops.clipboard_read) { lua_pushnil(L); return 1; }
    char buf[4096];
    size_t n = 0;
    int rc = eng->host_ops.clipboard_read(eng->host_ops_ud, buf, sizeof(buf), &n);
    if (rc != 1) { lua_pushnil(L); return 1; }
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    lua_pushlstring(L, buf, n);
    return 1;
}

static int host_clipboard_write(lua_State* L) {
    mdr_engine_t* eng = get_engine(L);
    if (!eng || !eng->host_ops.clipboard_write) { lua_pushboolean(L, 0); return 1; }
    size_t n = 0;
    const char* s = luaL_optlstring(L, 1, "", &n);
    int rc = eng->host_ops.clipboard_write(eng->host_ops_ud, s, n);
    lua_pushboolean(L, rc == 1);
    return 1;
}

static int host_read_selection(lua_State* L) {
    mdr_engine_t* eng = get_engine(L);
    if (!eng || !eng->host_ops.read_selection) { lua_pushnil(L); return 1; }
    char buf[4096];
    size_t n = 0;
    int rc = eng->host_ops.read_selection(eng->host_ops_ud, buf, sizeof(buf), &n);
    if (rc != 1) { lua_pushnil(L); return 1; }
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    lua_pushlstring(L, buf, n);
    return 1;
}

/* ---- modore.text.* pure baseline helpers ------------------------------ */

static int text_word_bounds(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    lua_Integer raw_caret = luaL_optinteger(L, 2, (lua_Integer)len);
    size_t caret = raw_caret < 0 ? 0 : (size_t)raw_caret;
    mdr_byte_bounds_t bounds = {0};
    if (mdr_text_word_bounds(s, len, caret, &bounds) != 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 2);
    lua_pushinteger(L, (lua_Integer)bounds.start_byte);
    lua_setfield(L, -2, "start_byte");
    lua_pushinteger(L, (lua_Integer)bounds.end_byte);
    lua_setfield(L, -2, "end_byte");
    return 1;
}

/* The split helpers all have the same shape — `(text, len, *split) -> rc`,
 * returning a single byte offset — and present the same two-string result to
 * Lua (head, tail). Share that wrapper; each Lua entry point just names its
 * mdr_text_* function. */
static int push_text_split(lua_State* L,
                           int (*split_fn)(const char*, size_t, size_t*)) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    size_t split = 0;
    if (split_fn(s, len, &split) != 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, s, split);
    lua_pushlstring(L, s + split, len - split);
    return 2;
}

static int text_split_trailing_ascii(lua_State* L) {
    return push_text_split(L, mdr_text_split_trailing_ascii);
}

static int text_split_trailing_ascii_punctuation(lua_State* L) {
    return push_text_split(L, mdr_text_split_trailing_ascii_punctuation);
}

static int text_normalize_pickup_suffix(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    char out[256];
    size_t out_len = 0;
    if (mdr_text_normalize_pickup_suffix(s, len, out, sizeof(out), &out_len) != 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, out, out_len);
    return 1;
}

static int text_split_acronym_head(lua_State* L) {
    return push_text_split(L, mdr_text_split_acronym_head);
}

/* ---- one-time global registration ------------------------------------ */

void ms_register_modore_globals(mdr_engine_t* eng) {
    lua_State* L = eng->L;

    /* Stash engine pointer in registry. */
    lua_pushlightuserdata(L, eng);
    lua_setfield(L, LUA_REGISTRYINDEX, RK_ENGINE_PTR);

    /* Shared `modore.log` table. */
    lua_createtable(L, 0, 3);
    lua_pushcfunction(L, log_info);  lua_setfield(L, -2, "info");
    lua_pushcfunction(L, log_warn);  lua_setfield(L, -2, "warn");
    lua_pushcfunction(L, log_error); lua_setfield(L, -2, "error");
    lua_setfield(L, LUA_REGISTRYINDEX, RK_MODORE_LOG);

    /* Shared `modore.default` table. */
    lua_createtable(L, 0, 5);
    lua_pushcfunction(L, default_pickup);      lua_setfield(L, -2, "pickup");
    lua_pushcfunction(L, default_replacement); lua_setfield(L, -2, "replacement");
    lua_pushcfunction(L, default_route);       lua_setfield(L, -2, "route");
    lua_pushcfunction(L, default_acquire);     lua_setfield(L, -2, "acquire");
    lua_pushcfunction(L, default_candidates);  lua_setfield(L, -2, "candidates");
    lua_setfield(L, LUA_REGISTRYINDEX, RK_MODORE_DEFAULT);

    /* Shared `modore.host` table — imperative primitives for stage callbacks. */
    lua_createtable(L, 0, 5);
    lua_pushcfunction(L, host_send_chord);      lua_setfield(L, -2, "send_chord");
    lua_pushcfunction(L, host_sleep_ms);        lua_setfield(L, -2, "sleep_ms");
    lua_pushcfunction(L, host_clipboard_read);  lua_setfield(L, -2, "clipboard_read");
    lua_pushcfunction(L, host_clipboard_write); lua_setfield(L, -2, "clipboard_write");
    lua_pushcfunction(L, host_read_selection);  lua_setfield(L, -2, "read_selection");
    lua_setfield(L, LUA_REGISTRYINDEX, RK_MODORE_HOST);

    /* Shared `modore.text` table — pure helpers that mirror host baseline
     * span/pre-processing logic in the engine's UTF-8 byte domain. */
    lua_createtable(L, 0, 5);
    lua_pushcfunction(L, text_word_bounds);                      lua_setfield(L, -2, "word_bounds");
    lua_pushcfunction(L, text_split_trailing_ascii);             lua_setfield(L, -2, "split_trailing_ascii");
    lua_pushcfunction(L, text_split_trailing_ascii_punctuation); lua_setfield(L, -2, "split_trailing_ascii_punctuation");
    lua_pushcfunction(L, text_split_acronym_head);               lua_setfield(L, -2, "split_acronym_head");
    lua_pushcfunction(L, text_normalize_pickup_suffix);          lua_setfield(L, -2, "normalize_pickup_suffix");
    lua_setfield(L, LUA_REGISTRYINDEX, RK_MODORE_TEXT);
}

/* ---- per-script env --------------------------------------------------- */

/* Copy a global name from the base state into the env table on top of
 * stack. Silently skips if the global is nil (some Lua builds omit
 * features). */
static void copy_global(lua_State* L, int env_idx, const char* name) {
    lua_getglobal(L, name);
    if (!lua_isnil(L, -1)) {
        lua_setfield(L, env_idx, name);
    } else {
        lua_pop(L, 1);
    }
}

/* Copy a sub-field of a global into env. e.g. ("os","time") copies os.time
 * but only os.time. */
static void copy_global_field(lua_State* L, int env_idx,
                              const char* parent_name, const char* child_name,
                              const char* env_field) {
    lua_getglobal(L, parent_name);
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, child_name);
        if (!lua_isnil(L, -1)) {
            lua_setfield(L, env_idx, env_field);
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

int ms_build_env(mdr_engine_t* eng) {
    lua_State* L = eng->L;
    lua_createtable(L, 0, 32);
    int env = lua_gettop(L);

    /* Whitelisted globals. */
    static const char* SAFE_GLOBALS[] = {
        "pairs", "ipairs", "next", "select", "type",
        "tostring", "tonumber", "error", "assert",
        "pcall", "xpcall", "unpack", "rawequal", "rawget", "rawset",
        "string", "table", "math",
        NULL
    };
    for (const char** p = SAFE_GLOBALS; *p; ++p) copy_global(L, env, *p);

    /* Restricted os: time-related only. Build a fresh `os` table. */
    lua_createtable(L, 0, 4);
    copy_global_field(L, lua_gettop(L), "os", "time",     "time");
    copy_global_field(L, lua_gettop(L), "os", "date",     "date");
    copy_global_field(L, lua_gettop(L), "os", "clock",    "clock");
    copy_global_field(L, lua_gettop(L), "os", "difftime", "difftime");
    lua_setfield(L, env, "os");

    /* print → log.info */
    lua_pushcfunction(L, sandbox_print);
    lua_setfield(L, env, "print");

    /* Build per-script `modore` table. */
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, MDR_ABI_VERSION);
    lua_setfield(L, -2, "abi_version");
    lua_getfield(L, LUA_REGISTRYINDEX, RK_MODORE_LOG);
    lua_setfield(L, -2, "log");
    lua_getfield(L, LUA_REGISTRYINDEX, RK_MODORE_DEFAULT);
    lua_setfield(L, -2, "default");
    lua_getfield(L, LUA_REGISTRYINDEX, RK_MODORE_HOST);
    lua_setfield(L, -2, "host");
    lua_getfield(L, LUA_REGISTRYINDEX, RK_MODORE_TEXT);
    lua_setfield(L, -2, "text");
    /* Pre-set hook slots to nil — explicit nil makes the user-defined
     * absent state clear in diagnostics. Lua treats unset == nil anyway. */
    lua_pushnil(L); lua_setfield(L, -2, "on_pickup");
    lua_pushnil(L); lua_setfield(L, -2, "on_replacement");
    lua_pushnil(L); lua_setfield(L, -2, "route_for_app");
    lua_pushnil(L); lua_setfield(L, -2, "on_candidates");
    lua_pushnil(L); lua_setfield(L, -2, "on_acquire");
    lua_setfield(L, env, "modore");

    /* Park env in the registry; return ref. */
    lua_pushvalue(L, env);
    int env_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);  /* pop env */
    return env_ref;
}
