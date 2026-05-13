/* engine.c — modore_script_t lifecycle (Phase 01 stub).
 *
 * Wraps a single LuaJIT lua_State. Phase 02 layers on hook registration,
 * sandbox loader, mtime-poll reload, log callback plumbing, and the
 * modore.* Lua namespace. For now: init creates an empty state with
 * standard libs opened; shutdown closes it.
 */

#include "modore_script.h"

#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

struct modore_script {
    lua_State* L;
};

modore_script_t* modore_script_init(void) {
    modore_script_t* eng = (modore_script_t*)calloc(1, sizeof(*eng));
    if (!eng) return NULL;

    eng->L = luaL_newstate();
    if (!eng->L) {
        free(eng);
        return NULL;
    }
    luaL_openlibs(eng->L);
    return eng;
}

void modore_script_shutdown(modore_script_t* eng) {
    if (!eng) return;
    if (eng->L) lua_close(eng->L);
    free(eng);
}

int modore_script_abi_version(void) {
    return MODORE_SCRIPT_ABI_VERSION;
}
