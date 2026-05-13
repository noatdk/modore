/* modore_script — Lua scripting engine for modore (host-loaded shared lib).
 *
 * Phase 01: stub init/shutdown only. The full versioned ABI lands in Phase 02
 * (hooks, log callback, host-default trampolines, marshaling rules). This
 * header exists now so the host build system has something to consume and
 * the engine has somewhere to put its first symbol.
 *
 * Trust model: scripts run with local-user trust. Sandbox is a deterrent
 * against accidental footgun usage of `io`/`os.execute`/`ffi`, not a
 * security boundary.
 *
 * Threading: a single modore_script_t* is owned by one thread. Hosts that
 * call from multiple threads must serialize externally.
 */

#ifndef MODORE_SCRIPT_H
#define MODORE_SCRIPT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef MODORE_SCRIPT_BUILDING
#    define MODORE_SCRIPT_EXPORT __declspec(dllexport)
#  else
#    define MODORE_SCRIPT_EXPORT __declspec(dllimport)
#  endif
#else
#  define MODORE_SCRIPT_EXPORT __attribute__((visibility("default")))
#endif

#define MODORE_SCRIPT_ABI_VERSION 1

typedef struct modore_script modore_script_t;

/* Returns NULL on failure. */
MODORE_SCRIPT_EXPORT modore_script_t* modore_script_init(void);

/* Idempotent: shutdown(NULL) is a no-op. */
MODORE_SCRIPT_EXPORT void modore_script_shutdown(modore_script_t* engine);

/* Returns MODORE_SCRIPT_ABI_VERSION as compiled into the .dylib/.so. Hosts
 * compare against the header constant at load time to refuse mismatched libs. */
MODORE_SCRIPT_EXPORT int modore_script_abi_version(void);

#ifdef __cplusplus
}
#endif

#endif /* MODORE_SCRIPT_H */
