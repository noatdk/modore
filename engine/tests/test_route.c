/* test_route.c — opt-in matrix for route_for_app. */

#include "test_helpers.h"

static void run_state(const char* label, const char* lua_body,
                      int expect_rc, mdr_route_t expect_route) {
    char* dir = test_mkdtemp();
    test_write(dir, "default.lua", lua_body);

    mdr_engine_t* eng = mdr_init();
    mdr_set_log_callback(eng, test_log_cb, NULL);
    mdr_load_dir(eng, dir);

    mdr_route_t r = MDR_ROUTE_DEFAULT;
    test_reset_log();
    mdr_pickup_ctx_t ctx = { .app_id = "com.example" };
    int rc = mdr_route(eng, &ctx, &r);

    char msg[128];
    snprintf(msg, sizeof(msg), "%s: rc", label);
    CHECK(rc == expect_rc, msg);
    if (expect_rc == 1) {
        snprintf(msg, sizeof(msg), "%s: route", label);
        CHECK(r == expect_route, msg);
    }

    mdr_shutdown(eng);
    test_cleanup_dir(dir);
    free(dir);
}

int main(void) {
    run_state("undefined", "-- empty\n",                                 0, MDR_ROUTE_DEFAULT);
    run_state("value-str",
              "modore.route_for_app = function(a) return 'clipboard' end\n",
              1, MDR_ROUTE_CLIPBOARD);
    run_state("value-ax",
              "modore.route_for_app = function(a) return 'ax' end\n",
              1, MDR_ROUTE_AX);
    run_state("value-keystroke",
              "modore.route_for_app = function(a) return 'keystroke' end\n",
              1, MDR_ROUTE_KEYSTROKE);
    run_state("value-selection-sync",
              "modore.route_for_app = function(a) return 'selection_sync' end\n",
              1, MDR_ROUTE_SELECTION_SYNC);
    run_state("nil",
              "modore.route_for_app = function(a) return nil end\n",
              0, MDR_ROUTE_DEFAULT);
    run_state("error",
              "modore.route_for_app = function(a) error('boom') end\n",
              0, MDR_ROUTE_DEFAULT);
    CHECK(g_log_last_level == MDR_LOG_ERROR, "error case logged at ERROR");

    /* invalid string returns 0 (use default) */
    run_state("invalid",
              "modore.route_for_app = function(a) return 'tunnel' end\n",
              0, MDR_ROUTE_DEFAULT);

    /* Per-app match preferred over default */
    {
        char* dir = test_mkdtemp();
        test_write(dir, "default.lua",
                   "modore.route_for_app = function(a) return 'ax' end\n");
        test_write(dir, "md.obsidian.lua",
                   "modore.route_for_app = function(a) return 'clipboard' end\n");
        mdr_engine_t* eng = mdr_init();
        mdr_load_dir(eng, dir);
        mdr_route_t r = MDR_ROUTE_DEFAULT;
        mdr_pickup_ctx_t obsidian = { .app_id = "md.obsidian" };
        CHECK(mdr_route(eng, &obsidian, &r) == 1, "per-app: rc==1");
        CHECK(r == MDR_ROUTE_CLIPBOARD,                "per-app: clipboard");
        r = MDR_ROUTE_DEFAULT;
        mdr_pickup_ctx_t other = { .app_id = "com.other" };
        CHECK(mdr_route(eng, &other, &r) == 1,   "default-fallback: rc==1");
        CHECK(r == MDR_ROUTE_AX,                      "default-fallback: ax");
        mdr_shutdown(eng);
        test_cleanup_dir(dir);
        free(dir);
    }

    REPORT_AND_EXIT("test_route");
}
