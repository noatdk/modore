#include "modore_script.h"

#include <string.h>

static size_t clamp_to_utf8_boundary(const char* s, size_t len, size_t pos) {
    if (pos > len) pos = len;
    while (pos > 0 && pos < len && (((unsigned char)s[pos]) & 0xC0) == 0x80) --pos;
    return pos;
}

static size_t utf8_next(const char* s, size_t len, size_t pos) {
    if (pos >= len) return len;
    ++pos;
    while (pos < len && (((unsigned char)s[pos]) & 0xC0) == 0x80) ++pos;
    return pos;
}

static size_t utf8_prev(const char* s, size_t pos) {
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && (((unsigned char)s[pos]) & 0xC0) == 0x80) --pos;
    return pos;
}

static int is_ascii_ws_at(const char* s, size_t pos) {
    unsigned char c = (unsigned char)s[pos];
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_ascii_at(const char* s, size_t pos) {
    return ((unsigned char)s[pos]) < 0x80;
}

static unsigned int utf8_codepoint_at(const char* s, size_t len, size_t pos) {
    if (pos >= len) return 0;
    unsigned char c0 = (unsigned char)s[pos];
    if (c0 < 0x80) return c0;
    if ((c0 & 0xE0) == 0xC0 && pos + 1 < len) {
        unsigned char c1 = (unsigned char)s[pos + 1];
        if ((c1 & 0xC0) == 0x80) {
            return ((unsigned int)(c0 & 0x1F) << 6) |
                   (unsigned int)(c1 & 0x3F);
        }
    }
    if ((c0 & 0xF0) == 0xE0 && pos + 2 < len) {
        unsigned char c1 = (unsigned char)s[pos + 1];
        unsigned char c2 = (unsigned char)s[pos + 2];
        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
            return ((unsigned int)(c0 & 0x0F) << 12) |
                   ((unsigned int)(c1 & 0x3F) << 6) |
                   (unsigned int)(c2 & 0x3F);
        }
    }
    if ((c0 & 0xF8) == 0xF0 && pos + 3 < len) {
        unsigned char c1 = (unsigned char)s[pos + 1];
        unsigned char c2 = (unsigned char)s[pos + 2];
        unsigned char c3 = (unsigned char)s[pos + 3];
        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 &&
            (c3 & 0xC0) == 0x80) {
            return ((unsigned int)(c0 & 0x07) << 18) |
                   ((unsigned int)(c1 & 0x3F) << 12) |
                   ((unsigned int)(c2 & 0x3F) << 6) |
                   (unsigned int)(c3 & 0x3F);
        }
    }
    return 0;
}

static int is_japanese_at(const char* s, size_t len, size_t pos) {
    unsigned int cp = utf8_codepoint_at(s, len, pos);
    return (cp >= 0x3040 && cp <= 0x30FF) ||
           (cp >= 0x3400 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF);
}

static int is_convertible_ascii_punctuation(unsigned char c) {
    return c == '.' || c == ',' || c == '-';
}

static int japanese_punctuation_span_at(
    const char* text, size_t len, size_t punct_pos,
    mdr_byte_bounds_t* out_bounds) {
    if (punct_pos >= len ||
        !is_convertible_ascii_punctuation((unsigned char)text[punct_pos]) ||
        punct_pos == 0) {
        return 0;
    }

    size_t prev = utf8_prev(text, punct_pos);
    size_t next = utf8_next(text, len, punct_pos);
    if (!is_japanese_at(text, len, prev)) return 0;
    if (next < len && !is_japanese_at(text, len, next)) return 0;

    out_bounds->start_byte = punct_pos;
    out_bounds->end_byte = next;
    return 1;
}

static int is_ascii_alnum(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
}

static int is_upper_ascii(unsigned char c) { return c >= 'A' && c <= 'Z'; }
static int is_lower_ascii(unsigned char c) { return c >= 'a' && c <= 'z'; }
static int is_digit_ascii(unsigned char c) { return c >= '0' && c <= '9'; }

static int is_acronym_symbol(unsigned char c) {
    switch (c) {
        case '&': case '-': case '.': case '_': case '+':
        case '/': case ':': case '@': case '#':
            return 1;
        default:
            return 0;
    }
}

int mdr_text_word_bounds(
    const char* text, size_t len, size_t caret_byte,
    mdr_byte_bounds_t* out_bounds) {
    if (!text || !out_bounds) return -1;
    size_t caret = clamp_to_utf8_boundary(text, len, caret_byte);

    if (caret < len && japanese_punctuation_span_at(text, len, caret, out_bounds)) {
        return 0;
    }
    if (caret < len && is_japanese_at(text, len, caret)) {
        size_t next = utf8_next(text, len, caret);
        if (japanese_punctuation_span_at(text, len, next, out_bounds)) {
            return 0;
        }
    }
    if (caret > 0) {
        size_t prev = utf8_prev(text, caret);
        if (japanese_punctuation_span_at(text, len, prev, out_bounds)) {
            return 0;
        }
    }

    if (caret > 0 && caret < len) {
        size_t prev = utf8_prev(text, caret);
        if (is_ascii_at(text, prev) && !is_ascii_at(text, caret)) {
            size_t ascii_start = prev;
            while (ascii_start > 0) {
                size_t before = utf8_prev(text, ascii_start);
                if (is_ascii_ws_at(text, before)) break;
                if (!is_ascii_at(text, before)) break;
                ascii_start = before;
            }
            out_bounds->start_byte = ascii_start;
            out_bounds->end_byte = caret;
            return 0;
        }
    }

    size_t start = caret;
    while (start > 0) {
        size_t prev = utf8_prev(text, start);
        if (is_ascii_ws_at(text, prev)) break;
        if (start < len && is_ascii_at(text, prev) != is_ascii_at(text, start)) break;
        start = prev;
    }

    size_t end = caret;
    while (end < len) {
        if (is_ascii_ws_at(text, end)) break;
        if (end > 0) {
            size_t prev = utf8_prev(text, end);
            if (is_ascii_at(text, prev) != is_ascii_at(text, end)) break;
        }
        end = utf8_next(text, len, end);
    }

    if (start == end) {
        if (caret < len &&
            is_convertible_ascii_punctuation((unsigned char)text[caret]) &&
            caret > 0) {
            size_t prev = utf8_prev(text, caret);
            if (is_japanese_at(text, len, prev)) {
                out_bounds->start_byte = prev;
                out_bounds->end_byte = caret;
                return 0;
            }
        }
        if (caret < len) {
            start = caret;
            end = utf8_next(text, len, caret);
        } else if (caret > 0) {
            start = utf8_prev(text, caret);
            end = caret;
        }
    }

    out_bounds->start_byte = start;
    out_bounds->end_byte = end;
    return 0;
}

int mdr_text_split_trailing_ascii(
    const char* text, size_t len, size_t* out_split_byte) {
    if (!text || !out_split_byte) return -1;
    size_t split = len;
    while (split > 0) {
        size_t prev = utf8_prev(text, split);
        if (!is_ascii_at(text, prev)) break;
        split = prev;
    }
    *out_split_byte = split;
    return 0;
}

int mdr_text_split_trailing_ascii_punctuation(
    const char* text, size_t len, size_t* out_core_end_byte) {
    if (!text || !out_core_end_byte) return -1;
    size_t split = len;
    while (split > 0) {
        size_t prev = utf8_prev(text, split);
        if (!is_ascii_at(text, prev)) break;
        unsigned char c = (unsigned char)text[prev];
        if (is_ascii_alnum(c)) break;
        split = prev;
    }
    *out_core_end_byte = split;
    return 0;
}

int mdr_text_split_acronym_head(
    const char* text, size_t len, size_t* out_head_end_byte) {
    if (!text || !out_head_end_byte) return -1;
    *out_head_end_byte = 0;
    if (len < 2 || !is_upper_ascii((unsigned char)text[0])) return 0;

    size_t i = 1;
    int saw_non_letter = 0;
    while (i < len) {
        unsigned char c = (unsigned char)text[i];
        if (is_upper_ascii(c)) {
            ++i;
        } else if (is_digit_ascii(c) || is_acronym_symbol(c)) {
            saw_non_letter = 1;
            ++i;
        } else {
            break;
        }
    }

    if (i >= 2 && i < len && is_lower_ascii((unsigned char)text[i]) &&
        (i >= 3 || saw_non_letter)) {
        *out_head_end_byte = i;
    }
    return 0;
}

int mdr_text_normalize_pickup_suffix(
    const char* suffix, size_t suffix_len,
    char* out_buf, size_t out_cap, size_t* out_len) {
    if (!suffix || !out_buf || !out_len || out_cap == 0) return -1;
    size_t n = 0;
    for (size_t i = 0; i < suffix_len; ++i) {
        const char* repl = NULL;
        size_t repl_len = 0;
        switch ((unsigned char)suffix[i]) {
            case '.':
                repl = "\xE3\x80\x82";
                repl_len = 3;
                break;
            case ',':
                repl = "\xE3\x80\x81";
                repl_len = 3;
                break;
            case '-':
                repl = "\xE3\x83\xBC";
                repl_len = 3;
                break;
            default:
                repl = suffix + i;
                repl_len = 1;
                break;
        }
        if (n + repl_len >= out_cap) break;
        memcpy(out_buf + n, repl, repl_len);
        n += repl_len;
    }
    out_buf[n] = '\0';
    *out_len = n;
    return 0;
}
