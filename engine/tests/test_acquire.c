/* test_acquire.c — opt-in matrix for on_acquire + host primitive plumbing. */

#include "test_helpers.h"

/* ---- host primitive stubs --------------------------------------------- */

static int   g_send_chord_calls;
static char  g_send_chord_last[64];
static int   g_sleep_calls;
static unsigned g_sleep_last_ms;
static char  g_fake_clip[256];   /* writable clipboard */
static int   g_clip_writes;

static void stub_send_chord(void* ud, const char* chord) {
    (void)ud;
    g_send_chord_calls++;
    snprintf(g_send_chord_last, sizeof(g_send_chord_last), "%s", chord ? chord : "");
}
static void stub_sleep_ms(void* ud, unsigned ms) {
    (void)ud;
    g_sleep_calls++;
    g_sleep_last_ms = ms;
}
static int stub_clipboard_read(void* ud, char* out, size_t cap, size_t* out_len) {
    (void)ud;
    size_t n = strlen(g_fake_clip);
    if (n >= cap) n = cap - 1;
    memcpy(out, g_fake_clip, n);
    out[n] = '\0';
    *out_len = n;
    return 1;
}
static int stub_clipboard_write(void* ud, const char* text, size_t len) {
    (void)ud;
    g_clip_writes++;
    if (len >= sizeof(g_fake_clip)) len = sizeof(g_fake_clip) - 1;
    memcpy(g_fake_clip, text, len);
    g_fake_clip[len] = '\0';
    return 1;
}

static void reset_stubs(void) {
    g_send_chord_calls = 0;
    g_send_chord_last[0] = '\0';
    g_sleep_calls = 0;
    g_sleep_last_ms = 0;
    g_clip_writes = 0;
    g_fake_clip[0] = '\0';
}

/* ---- matrix ----------------------------------------------------------- */

static void run_state(const char* label, const char* lua_body,
                      int expect_rc, const char* expect_out) {
    char* dir = test_mkdtemp();
    test_write(dir, "default.lua", lua_body);

    mdr_engine_t* eng = mdr_init();
    mdr_set_log_callback(eng, test_log_cb, NULL);

    mdr_host_ops_t ops = {
        .send_chord = stub_send_chord,
        .sleep_ms = stub_sleep_ms,
        .clipboard_read = stub_clipboard_read,
        .clipboard_write = stub_clipboard_write,
    };
    mdr_set_host_ops(eng, &ops, NULL);
    mdr_load_dir(eng, dir);

    char out[256] = {0};
    size_t out_len = 0;
    test_reset_log();
    int rc = mdr_acquire(eng, NULL, out, sizeof(out), &out_len);

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
    reset_stubs();

    /* (a) undefined */
    run_state("undefined", "-- empty\n", 0, NULL);

    /* (b) returns string — must compose via host primitives */
    snprintf(g_fake_clip, sizeof(g_fake_clip), "%s", "hello-from-clipboard");
    reset_stubs();
    snprintf(g_fake_clip, sizeof(g_fake_clip), "%s", "hello-from-clipboard");
    run_state("value",
              "modore.on_acquire = function(ctx, api)\n"
              "  return api.default.acquire(ctx)\n"
              "end\n",
              1, "hello-from-clipboard");
    CHECK(g_send_chord_calls == 2,                  "send_chord called twice");
    CHECK(strcmp(g_send_chord_last, "cmd+c") == 0,  "last chord was cmd+c");
    CHECK(g_sleep_last_ms == 30,                    "sleep saw 30ms");

    /* (c) returns nil */
    run_state("nil",
              "modore.on_acquire = function(ctx) return nil end\n",
              0, NULL);

    /* (d) errors */
    run_state("error",
              "modore.on_acquire = function(ctx) error('boom') end\n",
              0, NULL);
    CHECK(g_log_last_level == MDR_LOG_ERROR, "error case logged at ERROR");

    /* clipboard_write round-trip */
    reset_stubs();
    {
        char* dir = test_mkdtemp();
        test_write(dir, "default.lua",
                   "modore.on_acquire = function(ctx)\n"
                   "  modore.host.clipboard_write('wrote-from-script')\n"
                   "  return modore.host.clipboard_read()\n"
                   "end\n");
        mdr_engine_t* eng = mdr_init();
        mdr_set_log_callback(eng, test_log_cb, NULL);
        mdr_host_ops_t ops = {
            .send_chord = stub_send_chord,
            .sleep_ms = stub_sleep_ms,
            .clipboard_read = stub_clipboard_read,
            .clipboard_write = stub_clipboard_write,
        };
        mdr_set_host_ops(eng, &ops, NULL);
        mdr_load_dir(eng, dir);

        char out[64] = {0};
        size_t n = 0;
        int rc = mdr_acquire(eng, NULL, out, sizeof(out), &n);
        CHECK(rc == 1,                                  "round-trip: rc==1");
        CHECK(strcmp(out, "wrote-from-script") == 0,    "round-trip: read sees write");
        CHECK(g_clip_writes == 1,                       "clipboard_write fired once");
        mdr_shutdown(eng);
        test_cleanup_dir(dir);
        free(dir);
    }

    /* NULL host_ops → primitives return nil / no-op, but on_acquire can
     * still return a literal string. */
    {
        char* dir = test_mkdtemp();
        test_write(dir, "default.lua",
                   "modore.on_acquire = function(ctx)\n"
                   "  if modore.host.clipboard_read() == nil then return 'no-host' end\n"
                   "  return nil\n"
                   "end\n");
        mdr_engine_t* eng = mdr_init();
        mdr_set_log_callback(eng, test_log_cb, NULL);
        /* deliberately don't call mdr_set_host_ops */
        mdr_load_dir(eng, dir);
        char out[32] = {0};
        size_t n = 0;
        int rc = mdr_acquire(eng, NULL, out, sizeof(out), &n);
        CHECK(rc == 1,                          "no-host: rc==1");
        CHECK(strcmp(out, "no-host") == 0,      "no-host: literal returned");
        mdr_shutdown(eng);
        test_cleanup_dir(dir);
        free(dir);
    }

    REPORT_AND_EXIT("test_acquire");
}
