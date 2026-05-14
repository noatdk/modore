/* classifier.c — N-gram binary classifier for romaji/ASCII segmentation.
 *
 * Per the Ikegami & Tsuruta (2014) method: for each character position in
 * an ASCII string, extract character-surface and character-type n-gram
 * features in a window around that position, hash them into a fixed-size
 * bucket vector, and compute the logistic regression score. Positions
 * scoring above the threshold are labeled romaji; the rest ASCII.
 *
 * Additionally, a romaji-validity feature (greedy parse against Mozc's
 * own romaji table) provides a strong structural signal: whether the
 * current position falls inside a valid romaji syllable.
 *
 * Post-processing smooths the raw labels: uppercase / digit / symbol
 * positions are forced to ASCII, and runs shorter than 2 characters are
 * merged into their larger neighbor. */

#include "mdr_classifier.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----- Model structure (opaque to callers) ----------------------------- */

struct mdr_cls {
    uint32_t n_buckets;
    uint32_t ngram_max;
    uint32_t window;
    double   bias;
    double*  weights;   /* length = n_buckets */
    /* Optional English dictionary for boundary refinement. */
    char**   dict_words;
    size_t   dict_count;
};

/* ----- FNV-1a hash ----------------------------------------------------- */

static uint32_t fnv1a(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

/* ----- Character type -------------------------------------------------- */

static char char_type(char c) {
    if (c >= 'a' && c <= 'z') return 'L';
    if (c >= 'A' && c <= 'Z') return 'U';
    if (c >= '0' && c <= '9') return 'N';
    return 'S';
}

/* ----- Feature hashing ------------------------------------------------- */

static uint32_t hash_key(const char* key, size_t key_len, uint32_t n_buckets) {
    return fnv1a(key, key_len) % n_buckets;
}

/* Hash "{type_char}:{offset}/{gram}" */
static uint32_t hash_feature(char type_char, int offset,
                             const char* gram, int gram_len,
                             uint32_t n_buckets) {
    char buf[80];
    int pos = 0;
    buf[pos++] = type_char;
    buf[pos++] = ':';
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%d", offset);
    buf[pos++] = '/';
    if (gram_len > (int)(sizeof(buf) - (size_t)pos - 1))
        gram_len = (int)(sizeof(buf) - (size_t)pos - 1);
    memcpy(buf + pos, gram, (size_t)gram_len);
    pos += gram_len;
    return hash_key(buf, (size_t)pos, n_buckets);
}

/* ----- Romaji validity (greedy parse) --------------------------------- */

/* Compact table of valid romaji inputs from Mozc's romanji-hiragana.tsv.
 * Only the alphabetic entries that produce kana; sorted for bsearch. */
static const char* const ROMAJI_TABLE[] = {
    "a",    "ba",   "be",   "bi",   "bo",   "bu",   "bya",  "bye",
    "byi",  "byo",  "byu",  "cha",  "che",  "chi",  "cho",  "chu",
    "cya",  "cye",  "cyi",  "cyo",  "cyu",  "da",   "de",   "dha",
    "dhe",  "dhi",  "dho",  "dhu",  "di",   "do",   "du",   "dya",
    "dye",  "dyi",  "dyo",  "dyu",  "e",    "fa",   "fe",   "fi",
    "fo",   "fu",   "fya",  "fye",  "fyi",  "fyo",  "fyu",  "ga",
    "ge",   "gi",   "go",   "gu",   "gya",  "gye",  "gyi",  "gyo",
    "gyu",  "ha",   "he",   "hi",   "ho",   "hu",   "hya",  "hye",
    "hyi",  "hyo",  "hyu",  "i",    "ja",   "je",   "ji",   "jo",
    "ju",   "jya",  "jye",  "jyi",  "jyo",  "jyu",  "ka",   "ke",
    "ki",   "ko",   "ku",   "kya",  "kye",  "kyi",  "kyo",  "kyu",
    "la",   "le",   "li",   "lo",   "lu",   "lya",  "lye",  "lyi",
    "lyo",  "lyu",  "ma",   "me",   "mi",   "mo",   "mu",   "mya",
    "mye",  "myi",  "myo",  "myu",  "n",    "na",   "ne",   "ni",
    "nn",   "no",   "nu",   "nya",  "nye",  "nyi",  "nyo",  "nyu",
    "o",    "pa",   "pe",   "pi",   "po",   "pu",   "pya",  "pye",
    "pyi",  "pyo",  "pyu",  "ra",   "re",   "ri",   "ro",   "ru",
    "rya",  "rye",  "ryi",  "ryo",  "ryu",  "sa",   "se",   "sha",
    "she",  "shi",  "sho",  "shu",  "si",   "so",   "su",   "sya",
    "sye",  "syi",  "syo",  "syu",  "ta",   "te",   "tha",  "the",
    "thi",  "tho",  "thu",  "ti",   "to",   "tsa",  "tse",  "tsi",
    "tso",  "tsu",  "tu",   "tya",  "tye",  "tyi",  "tyo",  "tyu",
    "u",    "va",   "ve",   "vi",   "vo",   "vu",   "vya",  "vye",
    "vyi",  "vyo",  "vyu",  "wa",   "we",   "wi",   "wo",   "wu",
    "xa",   "xe",   "xi",   "xo",   "xu",   "xya",  "xye",  "xyi",
    "xyo",  "xyu",  "ya",   "ye",   "yi",   "yo",   "yu",   "za",
    "ze",   "zi",   "zo",   "zu",   "zya",  "zye",  "zyi",  "zyo",
    "zyu",
};
#define ROMAJI_TABLE_SIZE (sizeof(ROMAJI_TABLE) / sizeof(ROMAJI_TABLE[0]))

static int romaji_table_match(const char* s, size_t slen) {
    for (size_t i = 0; i < ROMAJI_TABLE_SIZE; i++) {
        size_t rlen = strlen(ROMAJI_TABLE[i]);
        if (rlen == slen && memcmp(ROMAJI_TABLE[i], s, slen) == 0)
            return 1;
    }
    return 0;
}

/* Greedy longest-match romaji parse. Sets covered[i]=1 for positions
 * inside a matched romaji syllable. */
static void romaji_coverage(const char* text, size_t len, uint8_t* covered) {
    memset(covered, 0, len);
    size_t i = 0;
    while (i < len) {
        int best = 0;
        /* Longest match first (max romaji input is 3 chars) */
        for (int try_len = (int)(len - i < 4 ? len - i : 4); try_len >= 1; try_len--) {
            char lower[5];
            for (int j = 0; j < try_len; j++)
                lower[j] = (text[i + j] >= 'A' && text[i + j] <= 'Z')
                           ? (char)(text[i + j] + 32)
                           : text[i + j];
            if (romaji_table_match(lower, (size_t)try_len)) {
                best = try_len;
                break;
            }
            /* 'n' before consonant or at end → ん */
            if (try_len == 1 && lower[0] == 'n') {
                if (i + 1 >= len) { best = 1; break; }
                char next = text[i + 1];
                if (next >= 'A' && next <= 'Z') next = (char)(next + 32);
                if (next != 'a' && next != 'i' && next != 'u' &&
                    next != 'e' && next != 'o' && next != 'y') {
                    best = 1;
                    break;
                }
            }
            /* Doubled consonant → っ (e.g. tt, kk, ss, pp, cc) */
            if (try_len == 1 && lower[0] >= 'b' && lower[0] <= 'z' &&
                lower[0] != 'n' && i + 1 < len) {
                char next = text[i + 1];
                if (next >= 'A' && next <= 'Z') next = (char)(next + 32);
                if (next == lower[0]) {
                    best = 1;
                    break;
                }
            }
        }
        if (best > 0) {
            for (int j = 0; j < best; j++)
                covered[i + (size_t)j] = 1;
            i += (size_t)best;
        } else {
            i++;
        }
    }
}

/* Forward declarations for dictionary lookup (defined after model I/O). */
static int dict_contains(const mdr_cls_t* cls,
                         const char* word, size_t wlen);
static int dict_is_prefix(const mdr_cls_t* cls,
                          const char* word, size_t wlen);

/* ----- Per-position score ---------------------------------------------- */

static double score_position(const mdr_cls_t* cls,
                             const char* text, size_t len, size_t pos,
                             const uint8_t* validity,
                             const uint8_t* history, size_t n_history) {
    double score = cls->bias;
    int W = (int)cls->window;
    int N = (int)cls->ngram_max;

    for (int off = -W; off <= W; off++) {
        for (int n = 1; n <= N; n++) {
            int start = (int)pos + off;
            int end   = start + n;
            if (start < 0 || end > (int)len) continue;

            /* Surface n-gram */
            uint32_t h = hash_feature('S', off, text + start, n, cls->n_buckets);
            score += cls->weights[h];

            /* Type n-gram */
            char tgram[16];
            for (int j = 0; j < n; j++)
                tgram[j] = char_type(text[start + j]);
            h = hash_feature('T', off, tgram, n, cls->n_buckets);
            score += cls->weights[h];
        }
    }

    /* Romaji-validity feature */
    if (validity && validity[pos]) {
        static const char VKEY[] = "V:covered";
        uint32_t h = hash_key(VKEY, sizeof(VKEY) - 1, cls->n_buckets);
        score += cls->weights[h];
    }

    /* History features: n-grams over previous classification labels.
     * Labels are encoded as 'K' (romaji/kana) or 'A' (ASCII). */
    if (history && n_history > 0) {
        char hbuf[16];
        int H = (int)(n_history < MDR_CLS_HISTORY ? n_history : MDR_CLS_HISTORY);
        for (int n = 1; n <= H; n++) {
            int hstart = (int)n_history - n;
            if (hstart < 0) break;
            for (int j = 0; j < n; j++)
                hbuf[j] = history[hstart + j] ? 'K' : 'A';
            char key[24];
            int klen = snprintf(key, sizeof(key), "H:%d/", -n);
            memcpy(key + klen, hbuf, (size_t)n);
            klen += n;
            uint32_t h = hash_key(key, (size_t)klen, cls->n_buckets);
            score += cls->weights[h];
        }
    }

    /* Dictionary features: does the ASCII run ending at pos form a
     * known English word or prefix? */
    if (cls->dict_words && cls->dict_count > 0) {
        /* Walk backwards to find start of ASCII alpha run */
        size_t run_start = pos;
        while (run_start > 0 && text[run_start - 1] >= 'A' &&
               ((text[run_start - 1] >= 'a' && text[run_start - 1] <= 'z') ||
                (text[run_start - 1] >= 'A' && text[run_start - 1] <= 'Z')))
            run_start--;
        size_t rlen = pos - run_start + 1;
        if (rlen >= 2 && rlen < 32) {
            char lower_run[32];
            for (size_t k = 0; k < rlen; k++)
                lower_run[k] = (text[run_start + k] >= 'A' &&
                                text[run_start + k] <= 'Z')
                               ? (char)(text[run_start + k] + 32)
                               : text[run_start + k];
            if (dict_contains(cls, lower_run, rlen)) {
                static const char DWORD[] = "D:word";
                uint32_t h = hash_key(DWORD, sizeof(DWORD) - 1, cls->n_buckets);
                score += cls->weights[h];
            }
            if (dict_is_prefix(cls, lower_run, rlen)) {
                static const char DPFX[] = "D:prefix";
                uint32_t h = hash_key(DPFX, sizeof(DPFX) - 1, cls->n_buckets);
                score += cls->weights[h];
            }
        }
    }

    return score;
}

static float sigmoid(double x) {
    return (float)(1.0 / (1.0 + exp(-x)));
}

/* ----- Model I/O ------------------------------------------------------- */

mdr_cls_t* mdr_cls_load(const char* path) {
    if (!path) return NULL;

    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    /* Read header */
    char magic[4];
    uint32_t version, n_buckets, ngram_max, window;
    double bias;

    if (fread(magic, 1, 4, f) != 4 ||
        memcmp(magic, MDR_CLS_MAGIC, 4) != 0) {
        fclose(f);
        return NULL;
    }
    if (fread(&version,   sizeof(uint32_t), 1, f) != 1 ||
        fread(&n_buckets, sizeof(uint32_t), 1, f) != 1 ||
        fread(&ngram_max, sizeof(uint32_t), 1, f) != 1 ||
        fread(&window,    sizeof(uint32_t), 1, f) != 1 ||
        fread(&bias,      sizeof(double),   1, f) != 1) {
        fclose(f);
        return NULL;
    }
    if (version != MDR_CLS_VERSION || n_buckets == 0 || n_buckets > 1000000) {
        fclose(f);
        return NULL;
    }

    /* Read weights */
    double* weights = (double*)calloc(n_buckets, sizeof(double));
    if (!weights) { fclose(f); return NULL; }

    if (fread(weights, sizeof(double), n_buckets, f) != n_buckets) {
        free(weights);
        fclose(f);
        return NULL;
    }
    fclose(f);

    mdr_cls_t* cls = (mdr_cls_t*)calloc(1, sizeof(*cls));
    if (!cls) { free(weights); return NULL; }

    cls->n_buckets = n_buckets;
    cls->ngram_max = ngram_max;
    cls->window    = window;
    cls->bias      = bias;
    cls->weights   = weights;
    return cls;
}

void mdr_cls_free(mdr_cls_t* cls) {
    if (!cls) return;
    free(cls->weights);
    if (cls->dict_words) {
        for (size_t i = 0; i < cls->dict_count; i++)
            free(cls->dict_words[i]);
        free(cls->dict_words);
    }
    free(cls);
}

/* ----- Dictionary for boundary refinement ------------------------------ */

int mdr_cls_load_dict(mdr_cls_t* cls, const char* dict_path) {
    if (!cls || !dict_path) return -1;

    FILE* f = fopen(dict_path, "r");
    if (!f) return -1;

    size_t cap = 4096;
    size_t count = 0;
    char** words = (char**)malloc(cap * sizeof(char*));
    if (!words) { fclose(f); return -1; }

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n < 2 || n > 30) continue;

        if (count == cap) {
            cap *= 2;
            char** tmp = (char**)realloc(words, cap * sizeof(char*));
            if (!tmp) break;
            words = tmp;
        }
        words[count] = (char*)malloc(n + 1);
        if (!words[count]) break;
        memcpy(words[count], line, n + 1);
        count++;
    }
    fclose(f);

    cls->dict_words = words;
    cls->dict_count = count;
    return 0;
}

static int dict_contains(const mdr_cls_t* cls,
                         const char* word, size_t wlen) {
    if (!cls->dict_words || cls->dict_count == 0) return 0;

    size_t lo = 0, hi = cls->dict_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strncmp(cls->dict_words[mid], word, wlen);
        if (cmp == 0) {
            size_t dlen = strlen(cls->dict_words[mid]);
            if (dlen == wlen) return 1;
            cmp = (dlen < wlen) ? -1 : 1;
        }
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

/* Check if `word` (length wlen) is a prefix of any dictionary entry. */
static int dict_is_prefix(const mdr_cls_t* cls,
                          const char* word, size_t wlen) {
    if (!cls->dict_words || cls->dict_count == 0) return 0;

    size_t lo = 0, hi = cls->dict_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strncmp(cls->dict_words[mid], word, wlen);
        if (cmp == 0) return 1;   /* dict word starts with this prefix */
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    /* Check if lo entry starts with word */
    if (lo < cls->dict_count && strncmp(cls->dict_words[lo], word, wlen) == 0)
        return 1;
    return 0;
}

/* ----- Classification -------------------------------------------------- */

/* Check if a substring can't convert to romaji at all (contains sequences
 * that are impossible in any romaji scheme). Used for the non-Japanese
 * dictionary heuristic. */
static int has_non_romaji_substring(const char* word, size_t wlen,
                                    const uint8_t* validity) {
    for (size_t i = 0; i < wlen; i++) {
        if (!validity[i]) return 1;
    }
    return 0;
}

int mdr_cls_classify(const mdr_cls_t* cls,
                     const char* text, size_t len,
                     uint8_t* out_labels) {
    if (!cls || !text || !out_labels) return -1;

    /* Pre-compute romaji validity for the entire string. */
    uint8_t* validity = (uint8_t*)malloc(len);
    if (!validity) return -1;
    romaji_coverage(text, len, validity);

    /* Auto-regressive: classify left-to-right, feeding previous labels
     * as history features for the next position. */
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        /* Hard rule: non-lowercase is always ASCII, except '-' after
         * romaji which represents chouon (ー) in Japanese input. */
        if (c < 'a' || c > 'z') {
            if (c == '-' && i > 0 && out_labels[i - 1] == 1)
                out_labels[i] = 1;
            else
                out_labels[i] = 0;
            continue;
        }
        double s = score_position(cls, text, len, i, validity,
                                  out_labels, i);
        out_labels[i] = (sigmoid(s) >= MDR_CLS_THRESHOLD) ? 1 : 0;
    }

    free(validity);
    return 0;
}

/* ----- Smoothing ------------------------------------------------------- */

/* Merge runs shorter than min_len into the preceding segment's label.
 * Operates in-place on the label array. */
static void smooth_labels(uint8_t* labels, size_t len, size_t min_len) {
    if (len < min_len) return;

    size_t run_start = 0;
    for (size_t i = 1; i <= len; i++) {
        if (i < len && labels[i] == labels[run_start]) continue;
        size_t run_len = i - run_start;
        if (run_len < min_len && run_start > 0) {
            uint8_t prev = labels[run_start - 1];
            for (size_t j = run_start; j < i; j++)
                labels[j] = prev;
        }
        run_start = i;
    }
}

static int starts_with_particle(const char* s, size_t len) {
    static const char* particles[] = {
        "de", "ni", "no", "wo", "ha", "ga", "to", "mo", "he", "ya",
        "kara", "made", "yori", NULL
    };
    for (const char** p = particles; *p; p++) {
        size_t plen = strlen(*p);
        if (len >= plen && strncmp(s, *p, plen) == 0) return 1;
    }
    return 0;
}

/* ----- Boundary refinement --------------------------------------------- */

/* At each ASCII→romaji boundary, check if the ASCII segment's text is
 * NOT a known English word. If trimming 1-6 chars from the end yields
 * a known word AND those chars begin a valid romaji syllable, shift the
 * boundary left. This fixes cases like [A:dockerdet][R:sukuru] →
 * [A:docker][R:detsukuru]. */
static void refine_boundaries(const mdr_cls_t* cls,
                              const char* text, size_t len,
                              uint8_t* labels) {
    if (!cls->dict_words || cls->dict_count == 0) return;

    size_t seg_start = 0;
    for (size_t i = 1; i <= len; i++) {
        if (i < len && labels[i] == labels[seg_start]) continue;
        size_t seg_len = i - seg_start;

        /* Refine ASCII segments (label 0) that are ≥4 chars:
         * either followed by romaji, or at end of string. */
        if (labels[seg_start] == 0 && seg_len >= 4 &&
            (i == len || labels[i] == 1)) {
            /* Convert segment to lowercase for lookup */
            char lower[64];
            if (seg_len < sizeof(lower)) {
                for (size_t k = 0; k < seg_len; k++)
                    lower[k] = (text[seg_start + k] >= 'A' &&
                                text[seg_start + k] <= 'Z')
                               ? (char)(text[seg_start + k] + 32)
                               : text[seg_start + k];

                /* Already a dictionary word? Keep as-is. */
                if (!dict_contains(cls, lower, seg_len)) {
                    size_t best_cand_len = 0;
                    /* Try trimming 1-6 chars from the end */
                    for (size_t trim = 1; trim <= 6 && trim < seg_len - 2; trim++) {
                        size_t cand_len = seg_len - trim;
                        if (dict_contains(cls, lower, cand_len)) {
                            if (best_cand_len == 0) best_cand_len = cand_len;
                            if (starts_with_particle(lower + cand_len, trim)) {
                                best_cand_len = cand_len;
                                break;
                            }
                        }
                    }
                    if (best_cand_len > 0) {
                        /* Shift: flip the trimmed chars to romaji */
                        for (size_t j = seg_start + best_cand_len; j < i; j++)
                            labels[j] = 1;
                    }
                }
            }
        }
        seg_start = i;
    }

    /* Pass 2: extend short ASCII segments forward into following romaji
     * when the combined text matches a dictionary word.
     * Fixes: [A:con][R:figwoijitte] → [A:config][R:woijitte] */
    seg_start = 0;
    for (size_t i = 1; i <= len; i++) {
        if (i < len && labels[i] == labels[seg_start]) continue;
        size_t seg_len = i - seg_start;

        if (labels[seg_start] == 0 && i < len && labels[i] == 1) {
            /* Find end of the following romaji segment */
            size_t rom_end = i;
            while (rom_end < len && labels[rom_end] == 1) rom_end++;

            /* Try extending ASCII by 1..min(10, romaji_len) chars */
            size_t rom_len = rom_end - i;
            size_t max_ext = rom_len < 10 ? rom_len : 10;
            char lower[64];
            size_t total = seg_len + max_ext;
            if (total < sizeof(lower)) {
                for (size_t k = 0; k < seg_len; k++)
                    lower[k] = (text[seg_start + k] >= 'A' &&
                                text[seg_start + k] <= 'Z')
                               ? (char)(text[seg_start + k] + 32)
                               : text[seg_start + k];

                size_t best_ext = 0;
                for (size_t ext = 1; ext <= max_ext; ext++) {
                    char c = text[i + ext - 1];
                    lower[seg_len + ext - 1] = (c >= 'A' && c <= 'Z')
                                                ? (char)(c + 32) : c;
                    if (ext >= 2 && dict_contains(cls, lower, seg_len + ext))
                        best_ext = ext;
                }
                if (best_ext > 0) {
                    for (size_t j = i; j < i + best_ext; j++)
                        labels[j] = 0;
                }
            }
        }
        seg_start = i;
    }
}

/* ----- Segmentation ---------------------------------------------------- */

/* Dictionary post-processing: force ASCII for segments that look like
 * English words — longer than 4 chars and containing non-romaji
 * substrings. Mirrors the paper's "Non-Japanese Dictionary" heuristic. */
static void dict_force_ascii(const char* text, size_t len,
                             uint8_t* labels) {
    uint8_t* validity = (uint8_t*)malloc(len);
    if (!validity) return;
    romaji_coverage(text, len, validity);

    size_t seg_start = 0;
    for (size_t i = 1; i <= len; i++) {
        if (i < len && labels[i] == labels[seg_start]) continue;
        size_t seg_len = i - seg_start;
        /* Only reconsider romaji segments ≥5 chars */
        if (labels[seg_start] == 1 && seg_len >= 5) {
            if (has_non_romaji_substring(text + seg_start, seg_len,
                                          validity + seg_start)) {
                for (size_t j = seg_start; j < i; j++)
                    labels[j] = 0;
            }
        }
        seg_start = i;
    }
    free(validity);
}

int mdr_cls_segment(const mdr_cls_t* cls,
                    const char* text, size_t len,
                    mdr_segment_t* out, size_t max_segments) {
    if (!cls || !text || !out || max_segments == 0) return -1;
    if (len == 0) return 0;

    /* 1) Per-character classification (auto-regressive) */
    uint8_t* labels = (uint8_t*)malloc(len);
    if (!labels) return -1;

    if (mdr_cls_classify(cls, text, len, labels) != 0) {
        free(labels);
        return -1;
    }

    /* 2) Smooth: merge runs shorter than 2 chars */
    smooth_labels(labels, len, 2);

    /* 3) Dictionary post-processing: force ASCII on romaji segments
     * that contain non-romaji substrings (English words). */
    dict_force_ascii(text, len, labels);

    /* Re-smooth after dictionary overrides */
    smooth_labels(labels, len, 2);

    /* 4) Dictionary-guided boundary refinement: fix splits where the
     * model put a Japanese particle on the ASCII side. */
    refine_boundaries(cls, text, len, labels);
    smooth_labels(labels, len, 2);

    /* 5) Build segments from contiguous same-label runs */
    int n_seg = 0;
    size_t seg_start = 0;
    for (size_t i = 1; i <= len; i++) {
        if (i < len && labels[i] == labels[seg_start]) continue;
        if ((size_t)n_seg >= max_segments) break;
        out[n_seg].start      = seg_start;
        out[n_seg].end        = i;
        out[n_seg].is_romaji  = labels[seg_start];
        n_seg++;
        seg_start = i;
    }

    free(labels);
    return n_seg;
}
