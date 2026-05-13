/* test_pickup.c — opt-in matrix for on_pickup.
 *
 * Four states per hook: undefined, returns value, returns nil, errors.
 * In all four, engine must distinguish only "use script result" (1) vs
 * "use default" (0). The host can't tell the latter three apart. */

#include "test_helpers.h"

static void run_state(const char* label, const char* lua_body,
                      int expect_rc, size_t expect_start, size_t expect_end) {
    char* dir = test_mkdtemp();
    test_write(dir, "default.lua", lua_body);

    mdr_engine_t* eng = mdr_init();
    mdr_set_log_callback(eng, test_log_cb, NULL);
    mdr_load_dir(eng, dir);

    mdr_pickup_ctx_t ctx = {
        .full_text     = "hello",
        .full_text_len = 5,
        .caret_byte    = 5,
        .app_id        = NULL,
        .flags         = 0,
    };
    mdr_span_t span = {0};
    test_reset_log();
    int rc = mdr_pickup(eng, &ctx, &span);

    char msg[128];
    snprintf(msg, sizeof(msg), "%s: rc", label);
    CHECK(rc == expect_rc, msg);
    if (expect_rc == 1) {
        snprintf(msg, sizeof(msg), "%s: span_start", label);
        CHECK(span.span_start_byte == expect_start, msg);
        snprintf(msg, sizeof(msg), "%s: span_end", label);
        CHECK(span.span_end_byte == expect_end, msg);
    }

    mdr_shutdown(eng);
    test_cleanup_dir(dir);
    free(dir);
}

int main(void) {
    /* (a) undefined: script doesn't assign modore.on_pickup. */
    run_state("undefined",
              "-- empty\n",
              0, 0, 0);

    /* (b) returns value */
    run_state("value",
              "modore.on_pickup = function(ctx)\n"
              "  return { start_byte = 0, end_byte = 5 }\n"
              "end\n",
              1, 0, 5);

    /* (c) returns nil */
    run_state("nil",
              "modore.on_pickup = function(ctx) return nil end\n",
              0, 0, 0);

    /* (d) errors */
    run_state("error",
              "modore.on_pickup = function(ctx) error('boom') end\n",
              0, 0, 0);
    CHECK(g_log_last_level == MDR_LOG_ERROR, "error case logged at ERROR");
    CHECK(strstr(g_log_last, "boom") != NULL, "log mentions 'boom'");

    REPORT_AND_EXIT("test_pickup");
}
