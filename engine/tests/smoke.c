/* smoke.c — minimal lifecycle check.
 *
 * Asserts: init returns non-null, abi_version matches header, shutdown
 * is idempotent on NULL. Exits 0 on success, 1 on failure. No external
 * test framework dep on purpose — keeps Phase 01 self-contained.
 */

#include "modore_script.h"

#include <stdio.h>
#include <stdlib.h>

static int fail(const char* msg) {
    fprintf(stderr, "smoke: FAIL %s\n", msg);
    return 1;
}

int main(void) {
    if (modore_script_abi_version() != MODORE_SCRIPT_ABI_VERSION) {
        return fail("abi_version mismatch with header");
    }

    modore_script_t* eng = modore_script_init();
    if (!eng) return fail("init returned NULL");

    modore_script_shutdown(eng);
    modore_script_shutdown(NULL); /* idempotent on NULL */

    printf("smoke: ok (abi=%d)\n", MODORE_SCRIPT_ABI_VERSION);
    return 0;
}
