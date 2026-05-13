/* test_engine.c — non-hook concerns:
 *   - "no script file" fast path
 *   - reload picks up new hook
 *   - reload that removes a hook reverts it to default
 *   - modore.default.* trampoline reaches host fn pointers
 *   - print() routes through log.info
 */

#include "test_helpers.h"
#include <unistd.h>

/* ---- host-default stubs --------------------------------------------- */

static int stub_def_pickup_called;
static int stub_def_pickup(void* ud, const mdr_pickup_ctx_t* ctx, mdr_span_t* out) {
    (void)ud; (void)ctx;
    stub_def_pickup_called++;
    out->span_start_byte = 7;
    out->span_end_byte   = 11;
    out->romaji = NULL;
    out->romaji_len = 0;
    return 1;
}

static int stub_def_route_called;
static int stub_def_route(void* ud, const char* app_id, mdr_route_t* out) {
    (void)ud; (void)app_id;
    stub_def_route_called++;
    *out = MDR_ROUTE_KEYSTROKE;
    return 1;
}

int main(void) {
    /* ---- no script file ---- */
    {
        mdr_engine_t* eng = mdr_init();
        mdr_set_log_callback(eng, test_log_cb, NULL);
        mdr_pickup_ctx_t ctx = {0};
        mdr_span_t span;
        CHECK(mdr_pickup(eng, &ctx, &span)            == 0, "no-script pickup");
        CHECK(mdr_route(eng, "x", &(mdr_route_t){0})  == 0, "no-script route");
        mdr_shutdown(eng);
    }

    /* ---- empty scripts dir ---- */
    {
        char* dir = test_mkdtemp();
        mdr_engine_t* eng = mdr_init();
        mdr_set_log_callback(eng, test_log_cb, NULL);
        mdr_load_dir(eng, dir);
        mdr_pickup_ctx_t ctx = {0};
        mdr_span_t span;
        CHECK(mdr_pickup(eng, &ctx, &span) == 0, "empty-dir pickup");
        mdr_shutdown(eng);
        rmdir(dir);
        free(dir);
    }

    /* ---- partial script: only on_replacement, others fall through ---- */
    {
        char* dir = test_mkdtemp();
        test_write(dir, "default.lua",
                   "modore.on_replacement = function(s,c) return 'X' end\n");
        mdr_engine_t* eng = mdr_init();
        mdr_set_log_callback(eng, test_log_cb, NULL);
        mdr_load_dir(eng, dir);

        mdr_pickup_ctx_t pctx = {0};
        mdr_span_t span = {0};
        CHECK(mdr_pickup(eng, &pctx, &span) == 0, "partial: pickup defaults");
        CHECK(mdr_route(eng, "x", &(mdr_route_t){0}) == 0, "partial: route defaults");

        char out[16] = {0}; size_t n = 0;
        CHECK(mdr_replacement(eng, NULL, &span, NULL, 0, out, sizeof(out), &n) == 1,
              "partial: replacement fires");
        CHECK(strcmp(out, "X") == 0, "partial: replacement value");
        mdr_shutdown(eng);
        test_cleanup_dir(dir);
        free(dir);
    }

    /* ---- reload: add a hook ---- */
    {
        char* dir = test_mkdtemp();
        char* path = test_write(dir, "default.lua", "-- empty\n");
        mdr_engine_t* eng = mdr_init();
        mdr_set_log_callback(eng, test_log_cb, NULL);
        mdr_load_dir(eng, dir);

        mdr_route_t r = MDR_ROUTE_DEFAULT;
        CHECK(mdr_route(eng, "x", &r) == 0, "reload: initially undefined");

        sleep(1); /* mtime granularity on macOS HFS+/APFS */
        FILE* f = fopen(path, "w");
        fputs("modore.route_for_app = function(a) return 'clipboard' end\n", f);
        fclose(f);

        r = MDR_ROUTE_DEFAULT;
        CHECK(mdr_route(eng, "x", &r) == 1,            "reload: now defined");
        CHECK(r == MDR_ROUTE_CLIPBOARD,                "reload: value picked up");

        mdr_shutdown(eng);
        test_cleanup_dir(dir);
        free(dir);
        free(path);
    }

    /* ---- reload: remove a hook reverts it to default ---- */
    {
        char* dir = test_mkdtemp();
        char* path = test_write(dir, "default.lua",
                                "modore.on_replacement = function(s,c) return 'Y' end\n");
        mdr_engine_t* eng = mdr_init();
        mdr_set_log_callback(eng, test_log_cb, NULL);
        mdr_load_dir(eng, dir);

        mdr_span_t span = {0};
        char out[16] = {0}; size_t n = 0;
        CHECK(mdr_replacement(eng, NULL, &span, NULL, 0, out, sizeof(out), &n) == 1,
              "remove-reload: pre-reload fires");
        CHECK(strcmp(out, "Y") == 0, "remove-reload: pre-reload value");

        sleep(1);
        FILE* f = fopen(path, "w");
        fputs("-- now empty\n", f);
        fclose(f);

        memset(out, 0, sizeof(out));
        CHECK(mdr_replacement(eng, NULL, &span, NULL, 0, out, sizeof(out), &n) == 0,
              "remove-reload: post-reload reverts to default");

        mdr_shutdown(eng);
        test_cleanup_dir(dir);
        free(dir);
        free(path);
    }

    /* ---- modore.default.* trampoline reaches host fn ptrs ---- */
    {
        char* dir = test_mkdtemp();
        test_write(dir, "default.lua",
                   "modore.on_pickup = function(ctx)\n"
                   "  local s = modore.default.pickup(ctx)\n"
                   "  if s then s.end_byte = s.end_byte + 1 end\n"
                   "  return s\n"
                   "end\n");
        mdr_engine_t* eng = mdr_init();
        mdr_set_log_callback(eng, test_log_cb, NULL);
        stub_def_pickup_called = 0;
        mdr_set_defaults(eng, NULL, stub_def_pickup, NULL, NULL);
        mdr_load_dir(eng, dir);

        mdr_pickup_ctx_t ctx = { .full_text = "x", .full_text_len = 1 };
        mdr_span_t span = {0};
        CHECK(mdr_pickup(eng, &ctx, &span) == 1, "trampoline: rc");
        CHECK(stub_def_pickup_called == 1,        "trampoline: default invoked");
        CHECK(span.span_start_byte == 7,          "trampoline: passes-through start");
        CHECK(span.span_end_byte   == 12,         "trampoline: script tweaked end");
        mdr_shutdown(eng);
        test_cleanup_dir(dir);
        free(dir);
    }

    /* ---- default.route trampoline ---- */
    {
        char* dir = test_mkdtemp();
        test_write(dir, "default.lua",
                   "modore.route_for_app = function(a)\n"
                   "  local r = modore.default.route(a)\n"
                   "  if r == 'keystroke' then return 'clipboard' end\n"
                   "  return r\n"
                   "end\n");
        mdr_engine_t* eng = mdr_init();
        mdr_set_log_callback(eng, test_log_cb, NULL);
        stub_def_route_called = 0;
        mdr_set_defaults(eng, NULL, NULL, NULL, stub_def_route);
        mdr_load_dir(eng, dir);

        mdr_route_t r = MDR_ROUTE_DEFAULT;
        CHECK(mdr_route(eng, "any", &r) == 1, "route-trampoline: rc");
        CHECK(stub_def_route_called == 1,      "route-trampoline: default invoked");
        CHECK(r == MDR_ROUTE_CLIPBOARD,        "route-trampoline: rewrite applied");
        mdr_shutdown(eng);
        test_cleanup_dir(dir);
        free(dir);
    }

    /* ---- print() routes to log.info ---- */
    {
        char* dir = test_mkdtemp();
        test_write(dir, "default.lua",
                   "modore.on_replacement = function(s,c)\n"
                   "  print('hello-from-print')\n"
                   "  return 'r'\n"
                   "end\n");
        mdr_engine_t* eng = mdr_init();
        mdr_set_log_callback(eng, test_log_cb, NULL);
        mdr_load_dir(eng, dir);

        mdr_span_t span = {0};
        char out[16] = {0}; size_t n = 0;
        test_reset_log();
        mdr_replacement(eng, NULL, &span, NULL, 0, out, sizeof(out), &n);
        CHECK(strstr(g_log_last, "hello-from-print") != NULL, "print routed to log");
        CHECK(g_log_last_level == MDR_LOG_INFO,                "print is INFO level");
        mdr_shutdown(eng);
        test_cleanup_dir(dir);
        free(dir);
    }

    REPORT_AND_EXIT("test_engine");
}
