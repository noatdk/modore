/* test_classifier.c — smoke tests for the n-gram romaji/ASCII classifier. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mdr_classifier.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); } \
} while (0)

/* ----- Model path discovery ------------------------------------------- */

static const char* find_model(void) {
    /* Look in engine/models/ relative to the build tree (CTest runs from
     * the engine build dir; the model lives in the source tree). */
    static const char* candidates[] = {
        "../../engine/models/classifier.mdl",
        "../engine/models/classifier.mdl",
        "engine/models/classifier.mdl",
        "models/classifier.mdl",
        NULL
    };
    for (const char** p = candidates; *p; p++) {
        FILE* f = fopen(*p, "rb");
        if (f) { fclose(f); return *p; }
    }
    return NULL;
}

/* ----- Tests ----------------------------------------------------------- */

static void test_load_missing(void) {
    mdr_cls_t* cls = mdr_cls_load("/nonexistent/path.mdl");
    CHECK(cls == NULL, "load missing file returns NULL");
}

static void test_load_invalid(void) {
    /* Write a garbage file and try to load it. */
    const char* path = "/tmp/modore-cls-test-invalid.mdl";
    FILE* f = fopen(path, "wb");
    if (f) {
        fputs("NOT_A_MODEL", f);
        fclose(f);
    }
    mdr_cls_t* cls = mdr_cls_load(path);
    CHECK(cls == NULL, "load invalid file returns NULL");
    unlink(path);
}

static void test_null_args(void) {
    CHECK(mdr_cls_segment(NULL, "abc", 3, NULL, 0) == -1,
          "segment with NULL cls returns -1");
    mdr_cls_free(NULL);  /* should not crash */
}

static void test_empty_input(void) {
    const char* model_path = find_model();
    if (!model_path) {
        fprintf(stderr, "  SKIP: model file not found\n");
        return;
    }
    mdr_cls_t* cls = mdr_cls_load(model_path);
    CHECK(cls != NULL, "model loaded");
    if (!cls) return;

    mdr_segment_t segs[4];
    int n = mdr_cls_segment(cls, "", 0, segs, 4);
    CHECK(n == 0, "empty input returns 0 segments");

    mdr_cls_free(cls);
}

static void test_uppercase_forced_ascii(void) {
    const char* model_path = find_model();
    if (!model_path) { fprintf(stderr, "  SKIP: model file not found\n"); return; }
    mdr_cls_t* cls = mdr_cls_load(model_path);
    if (!cls) return;

    /* Pure uppercase: all positions should be ASCII. */
    uint8_t labels[4];
    int rc = mdr_cls_classify(cls, "HTTP", 4, labels);
    CHECK(rc == 0, "classify HTTP returns 0");
    int all_ascii = 1;
    for (int i = 0; i < 4; i++) {
        if (labels[i] != 0) all_ascii = 0;
    }
    CHECK(all_ascii, "HTTP is all-ASCII");

    mdr_cls_free(cls);
}

static void test_segment_mixed(void) {
    const char* model_path = find_model();
    if (!model_path) { fprintf(stderr, "  SKIP: model file not found\n"); return; }
    mdr_cls_t* cls = mdr_cls_load(model_path);
    if (!cls) return;

    /* "APIkaitou" should segment into [ASCII:API] + [romaji:kaitou] */
    mdr_segment_t segs[8];
    int n = mdr_cls_segment(cls, "APIkaitou", 9, segs, 8);
    CHECK(n >= 2, "APIkaitou: at least 2 segments");
    if (n >= 2) {
        CHECK(segs[0].is_romaji == 0, "APIkaitou: first segment is ASCII");
        CHECK(segs[0].start == 0, "APIkaitou: first segment starts at 0");
        CHECK(segs[n-1].is_romaji == 1, "APIkaitou: last segment is romaji");
        CHECK(segs[n-1].end == 9, "APIkaitou: last segment ends at 9");
    }

    /* "areha8Bytedesu" should have at least 3 segments */
    n = mdr_cls_segment(cls, "areha8Bytedesu", 14, segs, 8);
    CHECK(n >= 2, "areha8Bytedesu: at least 2 segments");
    if (n >= 2) {
        /* The non-lowercase "8B" region should be ASCII */
        int found_ascii = 0;
        for (int i = 0; i < n; i++) {
            if (!segs[i].is_romaji) found_ascii = 1;
        }
        CHECK(found_ascii, "areha8Bytedesu: has an ASCII segment");
    }

    mdr_cls_free(cls);
}

static void test_segment_pure_romaji(void) {
    const char* model_path = find_model();
    if (!model_path) { fprintf(stderr, "  SKIP: model file not found\n"); return; }
    mdr_cls_t* cls = mdr_cls_load(model_path);
    if (!cls) return;

    mdr_segment_t segs[4];
    int n = mdr_cls_segment(cls, "nihongo", 7, segs, 4);
    CHECK(n >= 1, "nihongo: at least 1 segment");
    if (n == 1) {
        CHECK(segs[0].is_romaji == 1, "nihongo: single segment is romaji");
        CHECK(segs[0].start == 0 && segs[0].end == 7, "nihongo: covers full span");
    }

    mdr_cls_free(cls);
}

int main(void) {
    test_load_missing();
    test_load_invalid();
    test_null_args();
    test_empty_input();
    test_uppercase_forced_ascii();
    test_segment_mixed();
    test_segment_pure_romaji();

    fprintf(stderr, "test_classifier: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
