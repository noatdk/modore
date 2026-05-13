/* test_sandbox.c — confirm dangerous globals are absent from the script env. */

#include "test_helpers.h"

/* The script under test writes "1" or "0" into log when it sees / doesn't
 * see a global. We assert the log captured the expected outcome. */
static void check_global(const char* global_name, int should_be_visible) {
    char* dir = test_mkdtemp();
    char body[512];
    snprintf(body, sizeof(body),
        "modore.on_replacement = function(s,c)\n"
        "  if %s ~= nil then return '1' else return '0' end\n"
        "end\n",
        global_name);
    test_write(dir, "default.lua", body);

    mdr_engine_t* eng = mdr_init();
    mdr_set_log_callback(eng, test_log_cb, NULL);
    mdr_load_dir(eng, dir);

    mdr_span_t span = {0};
    char out[16] = {0};
    size_t out_len = 0;
    int rc = mdr_replacement(eng, NULL, &span, NULL, 0, out, sizeof(out), &out_len);

    char msg[128];
    snprintf(msg, sizeof(msg), "global '%s' rc==1", global_name);
    CHECK(rc == 1, msg);
    snprintf(msg, sizeof(msg), "global '%s' visibility", global_name);
    CHECK(out[0] == (should_be_visible ? '1' : '0'), msg);

    mdr_shutdown(eng);
    test_cleanup_dir(dir);
    free(dir);
}

int main(void) {
    /* Forbidden — must be nil in the sandbox. */
    check_global("io",         0);
    check_global("os.execute", 0);
    check_global("os.popen",   0);
    check_global("package",    0);
    check_global("require",    0);
    check_global("loadfile",   0);
    check_global("dofile",     0);
    check_global("load",       0);
    check_global("loadstring", 0);
    check_global("debug",      0);
    check_global("ffi",        0);
    check_global("setfenv",    0);
    check_global("getfenv",    0);

    /* Allowed — must be visible. */
    check_global("string",   1);
    check_global("table",    1);
    check_global("math",     1);
    check_global("pairs",    1);
    check_global("ipairs",   1);
    check_global("tostring", 1);
    check_global("os.time",  1);
    check_global("os.clock", 1);
    check_global("modore",   1);

    REPORT_AND_EXIT("test_sandbox");
}
