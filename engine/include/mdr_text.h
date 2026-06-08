/* mdr_text — shared romaji/ASCII text policy.
 *
 * Stateless UTF-8 helpers that own modore's baseline span/segmentation policy
 * (word bounds, trailing-ASCII / acronym splits, pickup-suffix normalization).
 * Implemented once in pickup_core.c and shared so the surfaces that pick text
 * apart do not drift:
 *   - libmodore_script (engine) exports them for the native hosts and Lua
 *     (`modore.text.*`);
 *   - libmozc_bridge compiles the same translation unit directly (with
 *     MDR_TEXT_INTERNAL, so its copy stays hidden) for the shell-native path,
 *     instead of re-implementing the policy in C++.
 *
 * Header is dependency-free (stddef.h only) so the bridge can include it
 * without pulling in the Lua engine ABI.
 */

#ifndef MODORE_MDR_TEXT_H
#define MODORE_MDR_TEXT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(MDR_TEXT_INTERNAL)
/* Compiled directly into a consumer (the bridge): keep this copy hidden on
 * ELF/Mach-O and avoid dllimport on Windows so the internal object file does
 * not turn into an import thunk. */
#  if defined(_WIN32) || defined(__CYGWIN__)
#    define MDR_TEXT_API
#  else
#    define MDR_TEXT_API __attribute__((visibility("hidden")))
#  endif
#elif defined(_WIN32) || defined(__CYGWIN__)
#  ifdef MDR_BUILDING
#    define MDR_TEXT_API __declspec(dllexport)
#  else
#    define MDR_TEXT_API __declspec(dllimport)
#  endif
#else
#  define MDR_TEXT_API __attribute__((visibility("default")))
#endif

typedef struct {
    size_t start_byte;
    size_t end_byte;
} mdr_byte_bounds_t;

MDR_TEXT_API int mdr_text_word_bounds(
    const char* text, size_t len, size_t caret_byte,
    mdr_byte_bounds_t* out_bounds);

MDR_TEXT_API int mdr_text_split_trailing_ascii(
    const char* text, size_t len, size_t* out_split_byte);

MDR_TEXT_API int mdr_text_split_trailing_ascii_punctuation(
    const char* text, size_t len, size_t* out_core_end_byte);

MDR_TEXT_API int mdr_text_split_acronym_head(
    const char* text, size_t len, size_t* out_head_end_byte);

MDR_TEXT_API int mdr_text_normalize_pickup_suffix(
    const char* suffix, size_t suffix_len,
    char* out_buf, size_t out_cap, size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif /* MODORE_MDR_TEXT_H */
