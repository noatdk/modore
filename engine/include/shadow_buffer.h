/* mdr_shadow_buffer — lightweight shadow edit buffer for key-log pickup.
 *
 * Tracks typed text as a UTF-8 string + cursor position so hosts can
 * extract the word at the caret at hotkey time without an AX or clipboard
 * round-trip.  Handles common editing gestures (arrows, backspace,
 * word-jump) so mid-word typo corrections do not reset the session.
 *
 * Design:
 *   - Platform layer maps native keycodes to the operations below.
 *   - Reset triggers: space, enter, tab, esc, mouse, focus-change,
 *     Cmd+non-nav, Ctrl+any.  Arrow keys are cursor moves, not resets.
 *   - Not thread-safe.  macOS host drives it from the CGEventTap callback;
 *     reads from other threads must go through a locking wrapper.
 *   - Word jump approximates macOS Option+Arrow (whitespace/punctuation
 *     boundaries).  Small cursor drift is tolerable — word_bounds at
 *     hotkey time re-anchors the span.
 */

#ifndef MDR_SHADOW_BUFFER_H
#define MDR_SHADOW_BUFFER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef MDR_BUILDING
#    define MDR_SB_EXPORT __declspec(dllexport)
#  else
#    define MDR_SB_EXPORT __declspec(dllimport)
#  endif
#else
#  define MDR_SB_EXPORT __attribute__((visibility("default")))
#endif

typedef struct mdr_shadow_buffer mdr_shadow_buffer_t;

/* Lifecycle ---------------------------------------------------------------- */

MDR_SB_EXPORT mdr_shadow_buffer_t* mdr_shadow_create(void);
MDR_SB_EXPORT void                 mdr_shadow_destroy(mdr_shadow_buffer_t*);

/* Editing operations ------------------------------------------------------- */

/* Insert UTF-8 bytes at the cursor. */
MDR_SB_EXPORT void mdr_shadow_insert(mdr_shadow_buffer_t*, const char* utf8, size_t len);

/* Delete the UTF-8 character immediately before the cursor (Backspace). */
MDR_SB_EXPORT void mdr_shadow_backspace(mdr_shadow_buffer_t*);

/* Delete the UTF-8 character at the cursor (Forward Delete). */
MDR_SB_EXPORT void mdr_shadow_forward_delete(mdr_shadow_buffer_t*);

/* Move cursor one code point left (delta=-1) or right (delta=+1). */
MDR_SB_EXPORT void mdr_shadow_move(mdr_shadow_buffer_t*, int delta);

/* Jump cursor one word left (forward=0) or right (forward=1).
 * Word = run of alphanumeric/underscore/high-byte characters. */
MDR_SB_EXPORT void mdr_shadow_word_jump(mdr_shadow_buffer_t*, int forward);

/* Jump cursor to start (forward=0) or end (forward=1) of buffer. */
MDR_SB_EXPORT void mdr_shadow_line_jump(mdr_shadow_buffer_t*, int forward);

/* Clear content and mark the buffer invalid.  Call on space, enter, esc,
 * mouse, focus-change, Cmd+non-nav, Ctrl, etc. */
MDR_SB_EXPORT void mdr_shadow_reset(mdr_shadow_buffer_t*);

/* Queries ------------------------------------------------------------------ */

/* 1 if buffer has accumulated content since the last reset, 0 otherwise. */
MDR_SB_EXPORT int    mdr_shadow_is_valid(mdr_shadow_buffer_t*);

/* UTF-8 text of the entire buffer.  *out_len = byte count (no NUL).
 * Pointer valid until the next mutating call.
 * Returns NULL when invalid or empty. */
MDR_SB_EXPORT const char* mdr_shadow_text(mdr_shadow_buffer_t*, size_t* out_len);

/* Cursor position as a byte offset into the text (0 = before first byte). */
MDR_SB_EXPORT size_t mdr_shadow_cursor_byte(mdr_shadow_buffer_t*);

#ifdef __cplusplus
}
#endif

#endif /* MDR_SHADOW_BUFFER_H */
