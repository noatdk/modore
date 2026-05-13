/* test_replacement.c — opt-in matrix for on_replacement. */

#include "test_helpers.h"

static void run_state(const char* label, const char* lua_body,
                      int expect_rc, const char* expect_out) {
    char* dir = test_mkdtemp();
    test_write(dir, "default.lua", lua_body);

    mdr_engine_t* eng = mdr_init();
    mdr_set_log_callback(eng, test_log_cb, NULL);
    mdr_load_dir(eng, dir);

    mdr_span_t span = { .span_start_byte = 0, .span_end_byte = 3,
                        .romaji = "abc", .romaji_len = 3 };
    const char* cands[] = { "甲", "乙", "丙" };
    char out[256] = {0};
    size_t out_len = 0;
    test_reset_log();
    int rc = mdr_replacement(eng, NULL, &span, cands, 3, out, sizeof(out), &out_len);

    char msg[128];
    snprintf(msg, sizeof(msg), "%s: rc", label);
    CHECK(rc == expect_rc, msg);
    if (expect_rc == 1) {
        snprintf(msg, sizeof(msg), "%s: out", label);
        CHECK(strcmp(out, expect_out) == 0, msg);
    }

    mdr_shutdown(eng);
    test_cleanup_dir(dir);
    free(dir);
}

int main(void) {
    run_state("undefined", "-- empty\n", 0, NULL);
    run_state("value",
              "modore.on_replacement = function(span, cands)\n"
              "  return cands[2]\n"
              "end\n",
              1, "乙");
    run_state("nil",
              "modore.on_replacement = function(s,c) return nil end\n",
              0, NULL);
    run_state("error",
              "modore.on_replacement = function(s,c) error('boom') end\n",
              0, NULL);
    CHECK(g_log_last_level == MDR_LOG_ERROR, "error case logged at ERROR");

    /* clamp: returning oversized string truncates without crash */
    {
        char* dir = test_mkdtemp();
        test_write(dir, "default.lua",
                   "modore.on_replacement = function(s,c)\n"
                   "  return string.rep('x', 10000)\n"
                   "end\n");
        mdr_engine_t* eng = mdr_init();
        mdr_set_log_callback(eng, test_log_cb, NULL);
        mdr_load_dir(eng, dir);
        mdr_span_t span = {0};
        char out[64] = {0};
        size_t n = 0;
        int rc = mdr_replacement(eng, NULL, &span, NULL, 0, out, sizeof(out), &n);
        CHECK(rc == 1, "clamp: rc==1");
        CHECK(n == sizeof(out) - 1, "clamp: out_len == cap-1");
        CHECK(out[sizeof(out) - 1] == '\0', "clamp: NUL terminated");
        mdr_shutdown(eng);
        test_cleanup_dir(dir);
        free(dir);
    }

    REPORT_AND_EXIT("test_replacement");
}
