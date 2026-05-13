/* smoke.c — minimal lifecycle check.
 *
 * Asserts: init returns non-null, abi_version matches header, shutdown
 * is idempotent on NULL. Exits 0 on success, 1 on failure. No external
 * test framework dep on purpose.
 */

#include "modore_script.h"

#include <stdio.h>
#include <stdlib.h>

static int fail(const char* msg) {
    fprintf(stderr, "smoke: FAIL %s\n", msg);
    return 1;
}

int main(void) {
    if (mdr_abi_version() != MDR_ABI_VERSION) {
        return fail("abi_version mismatch with header");
    }

    mdr_engine_t* eng = mdr_init();
    if (!eng) return fail("init returned NULL");

    mdr_shutdown(eng);
    mdr_shutdown(NULL); /* idempotent on NULL */

    printf("smoke: ok (abi=%d)\n", MDR_ABI_VERSION);
    return 0;
}
