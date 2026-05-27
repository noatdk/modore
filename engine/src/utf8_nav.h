#ifndef MODORE_UTF8_NAV_H
#define MODORE_UTF8_NAV_H

#include <stddef.h>

/* UTF-8 cursor navigation primitives shared by pickup_core.c and
 * shadow_buffer.c.  All functions are static inline so each translation
 * unit gets its own copy — no separate compilation unit required. */

static inline size_t utf8_prev(const char* s, size_t pos) {
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && (((unsigned char)s[pos]) & 0xC0) == 0x80) --pos;
    return pos;
}

static inline size_t utf8_next(const char* s, size_t len, size_t pos) {
    if (pos >= len) return len;
    ++pos;
    while (pos < len && (((unsigned char)s[pos]) & 0xC0) == 0x80) ++pos;
    return pos;
}

/* Snap an arbitrary byte offset back to the start of the code point that
 * contains it (no-op when pos is already on a lead byte or past end). */
static inline size_t utf8_clamp(const char* s, size_t len, size_t pos) {
    if (pos > len) pos = len;
    while (pos > 0 && pos < len && (((unsigned char)s[pos]) & 0xC0) == 0x80) --pos;
    return pos;
}

#endif /* MODORE_UTF8_NAV_H */
