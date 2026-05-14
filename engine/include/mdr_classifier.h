/* mdr_classifier — N-gram binary classifier for romaji/ASCII segmentation.
 *
 * Implements the core idea from Ikegami & Tsuruta (2014): per-character
 * binary classification using character-surface and character-type n-gram
 * features with logistic regression. Given a run of ASCII text, the
 * classifier labels each position as romaji (to be converted to kana) or
 * ASCII (to keep as-is), then groups contiguous same-label runs into
 * segments.
 *
 * Standalone: no dependency on the Lua scripting engine. Hosts load a
 * model file once at boot and call mdr_cls_segment on every pickup.
 *
 * Thread safety: a loaded mdr_cls_t is immutable after mdr_cls_load
 * returns; concurrent reads from any number of threads are safe.
 */

#ifndef MDR_CLASSIFIER_H
#define MDR_CLASSIFIER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef MDR_BUILDING
#    define MDR_EXPORT __declspec(dllexport)
#  else
#    define MDR_EXPORT __declspec(dllimport)
#  endif
#else
#  define MDR_EXPORT __attribute__((visibility("default")))
#endif

/* Opaque model handle. */
typedef struct mdr_cls mdr_cls_t;

/* A contiguous segment within an ASCII string. */
typedef struct {
    size_t start;       /* byte offset of first character */
    size_t end;         /* byte offset past last character */
    int    is_romaji;   /* 1 = romaji (convert), 0 = ASCII (keep) */
} mdr_segment_t;

/* ----- Lifecycle ------------------------------------------------------- */

/* Load a classifier model from a binary file.
 * Returns NULL on I/O error or format mismatch. */
MDR_EXPORT mdr_cls_t* mdr_cls_load(const char* model_path);

/* Load an English word dictionary for boundary refinement.
 * The file is a sorted text file with one lowercase word per line.
 * Returns 0 on success, -1 on error. NULL-safe (no-ops). */
MDR_EXPORT int mdr_cls_load_dict(mdr_cls_t* cls, const char* dict_path);

/* Free a loaded model. NULL-safe. */
MDR_EXPORT void mdr_cls_free(mdr_cls_t* cls);

/* ----- Inference ------------------------------------------------------- */

/* Segment an ASCII string into romaji and ASCII runs.
 *
 *   text:         input ASCII string (need not be NUL-terminated).
 *   len:          byte length of text.
 *   out:          caller-provided array of mdr_segment_t.
 *   max_segments: capacity of out[].
 *
 * Returns the number of segments written (>= 1 when len > 0), or
 *   0  if len == 0 (nothing to segment), or
 *  -1  on error (NULL args, cls not loaded).
 *
 * Hard rules applied before the learned model:
 *   - Uppercase, digit, and symbol positions are forced to ASCII.
 *   - Segments shorter than 2 characters are merged into their neighbor.
 */
MDR_EXPORT int mdr_cls_segment(
    const mdr_cls_t* cls,
    const char* text, size_t len,
    mdr_segment_t* out, size_t max_segments);

/* Per-character classification (exposed for testing / diagnostics).
 *
 *   out_labels: caller-provided array of at least `len` bytes.
 *               Each byte is set to 1 (romaji) or 0 (ASCII).
 *
 * Returns 0 on success, -1 on error. */
MDR_EXPORT int mdr_cls_classify(
    const mdr_cls_t* cls,
    const char* text, size_t len,
    uint8_t* out_labels);

/* ----- Model metadata -------------------------------------------------- */

/* Model file magic bytes. */
#define MDR_CLS_MAGIC "MDRC"

/* Current model format version. */
#define MDR_CLS_VERSION 1

/* Default hyper-parameters (can be overridden by model file). */
#define MDR_CLS_DEFAULT_BUCKETS   32768
#define MDR_CLS_DEFAULT_NGRAM_MAX 4
#define MDR_CLS_DEFAULT_WINDOW    4

/* History feature: number of previous labels fed back as n-gram features.
 * Following Ikegami & Tsuruta (2014) "History: KK, KK, KKK, ..." */
#define MDR_CLS_HISTORY           3

/* Classification threshold: P(romaji) must exceed this to be labeled
 * romaji. Biased slightly above 0.5 to prefer ASCII when uncertain —
 * a false-positive ASCII label just keeps English text intact, while
 * a false-positive romaji label mangles it through Mozc. */
#define MDR_CLS_THRESHOLD 0.55f

#ifdef __cplusplus
}
#endif

#endif /* MDR_CLASSIFIER_H */
