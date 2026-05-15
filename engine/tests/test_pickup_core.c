#include "test_helpers.h"

static void expect_bounds(
    const char* text,
    size_t caret,
    size_t want_start,
    size_t want_end,
    const char* label) {
    mdr_byte_bounds_t bounds = {0};
    CHECK(mdr_text_word_bounds(text, strlen(text), caret, &bounds) == 0, label);
    CHECK(bounds.start_byte == want_start, label);
    CHECK(bounds.end_byte == want_end, label);
}

static void check_bounds(void) {
    const char* text = "回答hentai";
    mdr_byte_bounds_t bounds = {0};
    CHECK(mdr_text_word_bounds(text, strlen(text), strlen(text), &bounds) == 0,
          "word_bounds rc");
    CHECK(bounds.start_byte == 6, "word_bounds start");
    CHECK(bounds.end_byte == 12, "word_bounds end");

    const char* jp_comma_end = "声,";
    expect_bounds(jp_comma_end, 0, 3, 4, "jp comma end: caret before kanji");
    expect_bounds(jp_comma_end, 3, 3, 4, "jp comma end: caret before comma");
    expect_bounds(jp_comma_end, 4, 3, 4, "jp comma end: caret after comma");

    const char* jp_period_end = "声.";
    expect_bounds(jp_period_end, 0, 3, 4, "jp period end: caret before kanji");
    expect_bounds(jp_period_end, 3, 3, 4, "jp period end: caret before period");
    expect_bounds(jp_period_end, 4, 3, 4, "jp period end: caret after period");

    const char* jp_hyphen_end = "声-";
    expect_bounds(jp_hyphen_end, 0, 3, 4, "jp hyphen end: caret before kanji");
    expect_bounds(jp_hyphen_end, 3, 3, 4, "jp hyphen end: caret before hyphen");
    expect_bounds(jp_hyphen_end, 4, 3, 4, "jp hyphen end: caret after hyphen");

    const char* jp_comma_jp = "声,次";
    expect_bounds(jp_comma_jp, 3, 3, 4, "jp comma jp: caret before comma");
    expect_bounds(jp_comma_jp, 4, 3, 4, "jp comma jp: caret after comma");

    const char* kana_comma_kana = "こ,え";
    expect_bounds(kana_comma_kana, 3, 3, 4, "kana comma kana: caret before comma");
    expect_bounds(kana_comma_kana, 4, 3, 4, "kana comma kana: caret after comma");

    const char* jp_comma_ascii = "声,abc";
    expect_bounds(jp_comma_ascii, 0, 0, 3, "jp comma ascii: caret before kanji");
    expect_bounds(jp_comma_ascii, 3, 0, 3, "jp comma ascii: caret before comma");

    const char* ascii_comma_jp = "abc,声";
    expect_bounds(ascii_comma_jp, 3, 0, 4, "ascii comma jp: caret before comma");
}

static void check_trailing_ascii(void) {
    const char* text = "対人sen";
    size_t split = 0;
    CHECK(mdr_text_split_trailing_ascii(text, strlen(text), &split) == 0,
          "split_trailing_ascii rc");
    CHECK(split == 6, "split_trailing_ascii split");
}

static void check_trailing_punctuation(void) {
    size_t split = 0;
    const char* text = "ge-mu-";
    CHECK(mdr_text_split_trailing_ascii_punctuation(text, strlen(text), &split) == 0,
          "split_trailing_ascii_punctuation rc");
    CHECK(split == 5, "split_trailing_ascii_punctuation split");

    text = "koe.";
    CHECK(mdr_text_split_trailing_ascii_punctuation(text, strlen(text), &split) == 0,
          "split trailing period rc");
    CHECK(split == 3, "split trailing period split");

    text = "koe,";
    CHECK(mdr_text_split_trailing_ascii_punctuation(text, strlen(text), &split) == 0,
          "split trailing comma rc");
    CHECK(split == 3, "split trailing comma split");

    text = ".";
    CHECK(mdr_text_split_trailing_ascii_punctuation(text, strlen(text), &split) == 0,
          "split punctuation-only rc");
    CHECK(split == 0, "split punctuation-only split");
}

static void check_acronym_head(void) {
    const char* text = "R&Diraisho";
    size_t split = 0;
    CHECK(mdr_text_split_acronym_head(text, strlen(text), &split) == 0,
          "split_acronym_head rc");
    CHECK(split == 3, "split_acronym_head split");
}

static void check_suffix_normalization(void) {
    char out[32] = {0};
    size_t out_len = 0;
    CHECK(mdr_text_normalize_pickup_suffix(".,-", 3, out, sizeof(out), &out_len) == 0,
          "normalize_pickup_suffix rc");
    CHECK(strcmp(out, "。、ー") == 0, "normalize_pickup_suffix value");
    CHECK(out_len == strlen("。、ー"), "normalize_pickup_suffix len");
}

int main(void) {
    check_bounds();
    check_trailing_ascii();
    check_trailing_punctuation();
    check_acronym_head();
    check_suffix_normalization();
    REPORT_AND_EXIT("test_pickup_core");
}
