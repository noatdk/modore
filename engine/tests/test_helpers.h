/* test_helpers.h — shared scaffolding for engine tests.
 *
 * Header-only so each test_*.c is self-contained. Provides:
 *   - tmp dir per test
 *   - write_script(dir, name, body)
 *   - log capture (last message + level)
 *   - PASS/FAIL counters + macros
 */

#ifndef MODORE_SCRIPT_TEST_HELPERS_H
#define MODORE_SCRIPT_TEST_HELPERS_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "modore_script.h"

static int   g_pass = 0;
static int   g_fail = 0;
static char  g_log_last[512] = {0};
static int   g_log_last_level = -1;
static int   g_log_count = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); } \
} while (0)

static int test_log_cb(void* ud, int level, const char* tag, const char* msg) {
    (void)ud; (void)tag;
    g_log_count++;
    g_log_last_level = level;
    snprintf(g_log_last, sizeof(g_log_last), "%s", msg ? msg : "");
    return 0;
}

static void test_reset_log(void) {
    g_log_last[0] = '\0';
    g_log_last_level = -1;
    g_log_count = 0;
}

/* Create a fresh temp dir. Returns malloc'd path; caller must free + rmdir
 * contents. Aborts on failure (tests are local-only). */
static char* test_mkdtemp(void) {
    char* tmpl = strdup("/tmp/modore-script-test.XXXXXX");
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); exit(2); }
    return tmpl;
}

/* Write `body` to `<dir>/<name>`. Returns full path (malloc'd). */
static char* test_write(const char* dir, const char* name, const char* body) {
    size_t n = strlen(dir) + 1 + strlen(name) + 1;
    char* p = (char*)malloc(n);
    snprintf(p, n, "%s/%s", dir, name);
    FILE* f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    if (body && *body) fputs(body, f);
    fclose(f);
    return p;
}

/* Remove a file (best-effort). */
static void test_unlink(const char* path) {
    if (unlink(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "unlink %s: %s\n", path, strerror(errno));
    }
}

/* Tear down dir; remove default.lua + per-app .lua if present. */
static void test_cleanup_dir(const char* dir) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s/default.lua", dir);
    unlink(buf);
    snprintf(buf, sizeof(buf), "%s/app.example.lua", dir);
    unlink(buf);
    snprintf(buf, sizeof(buf), "%s/md.obsidian.lua", dir);
    unlink(buf);
    rmdir(dir);
}

#define REPORT_AND_EXIT(name) do { \
    fprintf(stderr, "%s: %d pass, %d fail\n", (name), g_pass, g_fail); \
    return g_fail == 0 ? 0 : 1; \
} while (0)

#endif
