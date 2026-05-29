#include "shadow_buffer.h"
#include "utf8_nav.h"

#include <stdlib.h>
#include <string.h>

#define SB_INIT_CAP 128

struct mdr_shadow_buffer {
    char*  buf;     /* NUL-terminated UTF-8 */
    size_t len;     /* byte count, excluding NUL */
    size_t cap;     /* allocated bytes including NUL slot */
    size_t cursor;  /* byte offset in [0, len] */
    int    valid;   /* 1 after first insert since last reset */
};

/* Word character: alphanumeric, underscore, or multi-byte UTF-8 (CJK, etc.) */
static int sb_is_word_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

/* ---- Internal helpers ----------------------------------------------------- */

static int sb_grow(mdr_shadow_buffer_t* sb, size_t extra) {
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap) return 1;
    size_t ncap = sb->cap ? sb->cap * 2 : SB_INIT_CAP;
    while (ncap < need) ncap *= 2;
    char* p = realloc(sb->buf, ncap);
    if (!p) return 0;
    sb->buf = p;
    sb->cap = ncap;
    return 1;
}

/* ---- Lifecycle ------------------------------------------------------------ */

mdr_shadow_buffer_t* mdr_shadow_create(void) {
    mdr_shadow_buffer_t* sb = calloc(1, sizeof(*sb));
    if (!sb) return NULL;
    sb->buf = malloc(SB_INIT_CAP);
    if (!sb->buf) { free(sb); return NULL; }
    sb->buf[0] = '\0';
    sb->cap = SB_INIT_CAP;
    return sb;
}

void mdr_shadow_destroy(mdr_shadow_buffer_t* sb) {
    if (!sb) return;
    free(sb->buf);
    free(sb);
}

/* ---- Editing operations --------------------------------------------------- */

void mdr_shadow_insert(mdr_shadow_buffer_t* sb, const char* utf8, size_t len) {
    if (!sb || !utf8 || len == 0) return;
    if (!sb_grow(sb, len)) return;
    memmove(sb->buf + sb->cursor + len,
            sb->buf + sb->cursor,
            sb->len - sb->cursor + 1); /* +1 for NUL */
    memcpy(sb->buf + sb->cursor, utf8, len);
    sb->len    += len;
    sb->cursor += len;
    sb->valid   = 1;
}

void mdr_shadow_backspace(mdr_shadow_buffer_t* sb) {
    if (!sb || sb->cursor == 0) return;
    size_t prev = utf8_prev(sb->buf, sb->cursor);
    size_t del  = sb->cursor - prev;
    memmove(sb->buf + prev,
            sb->buf + sb->cursor,
            sb->len - sb->cursor + 1);
    sb->len    -= del;
    sb->cursor  = prev;
}

void mdr_shadow_forward_delete(mdr_shadow_buffer_t* sb) {
    if (!sb || sb->cursor >= sb->len) return;
    size_t next = utf8_next(sb->buf, sb->len, sb->cursor);
    size_t del  = next - sb->cursor;
    memmove(sb->buf + sb->cursor,
            sb->buf + next,
            sb->len - next + 1);
    sb->len -= del;
}

void mdr_shadow_move(mdr_shadow_buffer_t* sb, int delta) {
    if (!sb) return;
    if (delta < 0)
        sb->cursor = utf8_prev(sb->buf, sb->cursor);
    else if (delta > 0)
        sb->cursor = utf8_next(sb->buf, sb->len, sb->cursor);
}

void mdr_shadow_word_jump(mdr_shadow_buffer_t* sb, int forward) {
    if (!sb) return;
    if (forward) {
        /* skip non-word, then skip word characters */
        while (sb->cursor < sb->len &&
               !sb_is_word_byte((unsigned char)sb->buf[sb->cursor]))
            sb->cursor = utf8_next(sb->buf, sb->len, sb->cursor);
        while (sb->cursor < sb->len &&
               sb_is_word_byte((unsigned char)sb->buf[sb->cursor]))
            sb->cursor = utf8_next(sb->buf, sb->len, sb->cursor);
    } else {
        if (sb->cursor == 0) return;
        /* step back one, skip non-word, then skip word characters */
        size_t p = utf8_prev(sb->buf, sb->cursor);
        while (p > 0 && !sb_is_word_byte((unsigned char)sb->buf[p]))
            p = utf8_prev(sb->buf, p);
        if (!sb_is_word_byte((unsigned char)sb->buf[p])) { sb->cursor = 0; return; }
        while (p > 0) {
            size_t q = utf8_prev(sb->buf, p);
            if (!sb_is_word_byte((unsigned char)sb->buf[q])) break;
            p = q;
        }
        sb->cursor = p;
    }
}

void mdr_shadow_line_jump(mdr_shadow_buffer_t* sb, int forward) {
    if (!sb) return;
    sb->cursor = forward ? sb->len : 0;
}

void mdr_shadow_reset(mdr_shadow_buffer_t* sb) {
    if (!sb) return;
    sb->len    = 0;
    sb->cursor = 0;
    sb->valid  = 0;
    if (sb->buf) sb->buf[0] = '\0';
}

/* ---- Queries -------------------------------------------------------------- */

int mdr_shadow_is_valid(mdr_shadow_buffer_t* sb) {
    return (sb && sb->valid) ? 1 : 0;
}

const char* mdr_shadow_text(mdr_shadow_buffer_t* sb, size_t* out_len) {
    if (!sb || !sb->valid || sb->len == 0) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    if (out_len) *out_len = sb->len;
    return sb->buf;
}

size_t mdr_shadow_cursor_byte(mdr_shadow_buffer_t* sb) {
    return sb ? sb->cursor : 0;
}
