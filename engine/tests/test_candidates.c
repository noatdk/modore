/* test_candidates.c — opt-in matrix for on_candidates. */

#include "test_helpers.h"

static int count_strings(const char* buf, size_t buflen) {
    int n = 0;
    for (size_t i = 0; i < buflen; ++i) if (buf[i] == '\0') n++;
    return n;
}

static void run_state(const char* label, const char* lua_body,
                      int expect_rc, int expect_count, const char* expect_first) {
    char* dir = test_mkdtemp();
    test_write(dir, "default.lua", lua_body);

    mdr_engine_t* eng = mdr_init();
    mdr_set_log_callback(eng, test_log_cb, NULL);
    mdr_load_dir(eng, dir);

    const char* in[] = { "甲", "乙", "丙" };
    char out[256] = {0};
    size_t out_count = 0;
    test_reset_log();
    int rc = mdr_candidates(eng, NULL, in, 3, 0, out, sizeof(out), &out_count);

    char msg[128];
    snprintf(msg, sizeof(msg), "%s: rc", label);
    CHECK(rc == expect_rc, msg);
    if (expect_rc == 1) {
        snprintf(msg, sizeof(msg), "%s: count", label);
        CHECK((int)out_count == expect_count, msg);
        snprintf(msg, sizeof(msg), "%s: first", label);
        CHECK(strcmp(out, expect_first) == 0, msg);
        int actual = count_strings(out, sizeof(out));
        snprintf(msg, sizeof(msg), "%s: nul-separators", label);
        CHECK(actual >= expect_count, msg);
    }

    mdr_shutdown(eng);
    test_cleanup_dir(dir);
    free(dir);
}

int main(void) {
    run_state("undefined", "-- empty\n", 0, 0, NULL);
    run_state("value",
              "modore.on_candidates = function(c,i)\n"
              "  return { c[3], c[1] }\n"
              "end\n",
              1, 2, "丙");
    run_state("nil",
              "modore.on_candidates = function(c,i) return nil end\n",
              0, 0, NULL);
    run_state("error",
              "modore.on_candidates = function(c,i) error('boom') end\n",
              0, 0, NULL);
    CHECK(g_log_last_level == MDR_LOG_ERROR, "error case logged at ERROR");

    /* Empty table → use default */
    run_state("empty-table",
              "modore.on_candidates = function(c,i) return {} end\n",
              0, 0, NULL);

    REPORT_AND_EXIT("test_candidates");
}
