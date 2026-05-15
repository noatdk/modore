#include "test_helpers.h"

static int run_replacement_hook(const char* lua_body, char* out, size_t out_cap) {
    char* dir = test_mkdtemp();
    test_write(dir, "default.lua", lua_body);

    mdr_engine_t* eng = mdr_init();
    mdr_set_log_callback(eng, test_log_cb, NULL);
    mdr_load_dir(eng, dir);

    mdr_span_t span = {0};
    size_t out_len = 0;
    int rc = mdr_replacement(eng, NULL, &span, NULL, 0, out, out_cap, &out_len);

    mdr_shutdown(eng);
    test_cleanup_dir(dir);
    free(dir);
    return rc;
}

int main(void) {
    {
        char out[64] = {0};
        int rc = run_replacement_hook(
            "modore.on_replacement = function()\n"
            "  local b = modore.text.word_bounds('回答hentai', 12)\n"
            "  return b.start_byte .. ':' .. b.end_byte\n"
            "end\n",
            out, sizeof(out));
        CHECK(rc == 1, "word_bounds hook rc");
        CHECK(strcmp(out, "6:12") == 0, "word_bounds stops at ascii/non-ascii boundary");
    }

    {
        char out[64] = {0};
        int rc = run_replacement_hook(
            "modore.on_replacement = function()\n"
            "  local p, t = modore.text.split_trailing_ascii('対人sen')\n"
            "  return p .. '|' .. t\n"
            "end\n",
            out, sizeof(out));
        CHECK(rc == 1, "split_trailing_ascii hook rc");
        CHECK(strcmp(out, "対人|sen") == 0, "split_trailing_ascii value");
    }

    {
        char out[64] = {0};
        int rc = run_replacement_hook(
            "modore.on_replacement = function()\n"
            "  local h, t = modore.text.split_acronym_head('R&Diraisho')\n"
            "  return h .. '|' .. t\n"
            "end\n",
            out, sizeof(out));
        CHECK(rc == 1, "split_acronym_head hook rc");
        CHECK(strcmp(out, "R&D|iraisho") == 0, "split_acronym_head value");
    }

    {
        char out[64] = {0};
        int rc = run_replacement_hook(
            "modore.on_replacement = function()\n"
            "  local c, s = modore.text.split_trailing_ascii_punctuation('ge-mu,-')\n"
            "  return c .. '|' .. modore.text.normalize_pickup_suffix(s)\n"
            "end\n",
            out, sizeof(out));
        CHECK(rc == 1, "split_trailing_ascii_punctuation hook rc");
        CHECK(strcmp(out, "ge-mu|、ー") == 0, "normalize_pickup_suffix value");
    }

    REPORT_AND_EXIT("test_text");
}
