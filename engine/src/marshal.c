/* marshal.c — C struct ↔ Lua table marshaling for hook payloads.
 *
 * Strings are UTF-8 with explicit byte lengths. Tables are built eagerly
 * — hook hz is low (single-digit per second), so the per-call alloc
 * churn is invisible. */

#include "engine_internal.h"

#include <string.h>
#include <stdlib.h>

static void push_lstring_or_nil(lua_State* L, const char* s, size_t n) {
    if (!s) lua_pushnil(L);
    else    lua_pushlstring(L, s, n);
}

static void push_cstring_or_nil(lua_State* L, const char* s) {
    if (!s) lua_pushnil(L);
    else    lua_pushstring(L, s);
}

void ms_push_pickup_ctx(lua_State* L, const mdr_pickup_ctx_t* ctx) {
    lua_createtable(L, 0, 7);
    push_lstring_or_nil(L, ctx->full_text, ctx->full_text_len);
    lua_setfield(L, -2, "full_text");
    lua_pushinteger(L, (lua_Integer)ctx->caret_byte);
    lua_setfield(L, -2, "caret_byte");
    push_cstring_or_nil(L, ctx->app_id);
    lua_setfield(L, -2, "app_id");
    push_cstring_or_nil(L, ctx->field_role);
    lua_setfield(L, -2, "field_role");
    push_cstring_or_nil(L, ctx->field_description);
    lua_setfield(L, -2, "field_description");
    lua_pushboolean(L, (ctx->flags & 1u) != 0);
    lua_setfield(L, -2, "katakana");
    lua_pushinteger(L, (lua_Integer)ctx->flags);
    lua_setfield(L, -2, "flags");
}

void ms_push_span(lua_State* L, const mdr_span_t* span) {
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, (lua_Integer)span->span_start_byte);
    lua_setfield(L, -2, "start_byte");
    lua_pushinteger(L, (lua_Integer)span->span_end_byte);
    lua_setfield(L, -2, "end_byte");
    push_lstring_or_nil(L, span->romaji, span->romaji_len);
    lua_setfield(L, -2, "romaji");
}

void ms_push_cands(lua_State* L, const char* const* cands, size_t n) {
    lua_createtable(L, (int)n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (cands[i]) lua_pushstring(L, cands[i]);
        else          lua_pushliteral(L, "");
        lua_rawseti(L, -2, (int)(i + 1));
    }
}

/* Pull a span from a Lua table at idx into out. romaji_buf optional —
 * if NULL, out->romaji is set to NULL (caller copies later if needed).
 * Returns 1 on success. */
int ms_pull_span(lua_State* L, int idx, mdr_span_t* out, char* romaji_buf, size_t romaji_cap) {
    if (!lua_istable(L, idx)) return 0;
    int absidx = (idx > 0 || idx <= LUA_REGISTRYINDEX) ? idx : (lua_gettop(L) + 1 + idx);
    lua_getfield(L, absidx, "start_byte");
    lua_getfield(L, absidx, "end_byte");
    lua_getfield(L, absidx, "romaji");
    if (!lua_isnumber(L, -3) || !lua_isnumber(L, -2)) {
        lua_pop(L, 3);
        return 0;
    }
    out->span_start_byte = (size_t)lua_tointeger(L, -3);
    out->span_end_byte   = (size_t)lua_tointeger(L, -2);
    out->romaji = NULL;
    out->romaji_len = 0;
    if (lua_type(L, -1) == LUA_TSTRING && romaji_buf && romaji_cap > 0) {
        size_t rlen = 0;
        const char* rs = lua_tolstring(L, -1, &rlen);
        if (rlen >= romaji_cap) rlen = romaji_cap - 1;
        memcpy(romaji_buf, rs, rlen);
        romaji_buf[rlen] = '\0';
        out->romaji = romaji_buf;
        out->romaji_len = rlen;
    }
    lua_pop(L, 3);
    return 1;
}

int ms_pull_route(lua_State* L, int idx, mdr_route_t* out) {
    int t = lua_type(L, idx);
    if (t == LUA_TSTRING) {
        const char* s = lua_tostring(L, idx);
        if      (!strcmp(s, "default"))   *out = MDR_ROUTE_DEFAULT;
        else if (!strcmp(s, "ax"))        *out = MDR_ROUTE_AX;
        else if (!strcmp(s, "selection_sync") || !strcmp(s, "selection-sync"))
            *out = MDR_ROUTE_SELECTION_SYNC;
        else if (!strcmp(s, "keystroke")) *out = MDR_ROUTE_KEYSTROKE;
        else if (!strcmp(s, "clipboard")) *out = MDR_ROUTE_CLIPBOARD;
        else return 0;
        return 1;
    }
    if (t == LUA_TNUMBER) {
        int v = (int)lua_tointeger(L, idx);
        if (v < 0 || v > MDR_ROUTE_CLIPBOARD) return 0;
        *out = (mdr_route_t)v;
        return 1;
    }
    return 0;
}
