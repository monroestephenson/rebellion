#include "test.h"
#include "jsonval.h"

/* Parse a string literal by its exact byte length. */
#define JP(lit) rbl_jv_parse((lit), sizeof(lit) - 1)

void test_jsonval(void) {
    const char *txt =
        "{\"url\":\"https://x.de/\",\"engine\":\"browser\",\"onlyMainContent\":false,"
        "\"timeoutMs\":12000,\"actions\":[{\"type\":\"click\",\"selector\":\"#b\"},"
        "{\"type\":\"fill\",\"selector\":\"#q\",\"value\":\"laptop\"}],"
        "\"nested\":{\"a\":{\"b\":true}}}";
    rbl_jv *v = rbl_jv_parse(txt, strlen(txt));
    CHECK(v != NULL);
    if (!v) return;

    CHECK_STREQ(rbl_jv_get_str(v, "url", NULL), "https://x.de/");
    CHECK_STREQ(rbl_jv_get_str(v, "engine", NULL), "browser");
    CHECK(rbl_jv_get_bool(v, "onlyMainContent", true) == false);
    CHECK(rbl_jv_get_num(v, "timeoutMs", 0) == 12000.0);
    CHECK(rbl_jv_get_str(v, "missing", "def") != NULL);
    CHECK_STREQ(rbl_jv_get_str(v, "missing", "def"), "def");

    const rbl_jv *actions = rbl_jv_get(v, "actions");
    CHECK(actions && actions->type == RBL_JV_ARR);
    CHECK(actions && actions->count == 2);
    if (actions && actions->count == 2) {
        CHECK_STREQ(rbl_jv_get_str(actions->items[0], "type", NULL), "click");
        CHECK_STREQ(rbl_jv_get_str(actions->items[1], "value", NULL), "laptop");
    }

    /* nested object access */
    const rbl_jv *nested = rbl_jv_get(rbl_jv_get(v, "nested"), "a");
    CHECK(rbl_jv_get_bool(nested, "b", false) == true);

    rbl_jv_free(v);

    /* malformed inputs return NULL */
    CHECK(JP("{bad") == NULL);
    CHECK(JP("{\"a\":}") == NULL);
    CHECK(JP("[1,2,") == NULL);
    CHECK(JP("{} junk") == NULL);

    /* arrays and scalars */
    rbl_jv *arr = JP("[1, 2, 3]");
    CHECK(arr && arr->type == RBL_JV_ARR && arr->count == 3);
    if (arr && arr->count == 3) CHECK(rbl_jv_num(arr->items[2], 0) == 3.0);
    rbl_jv_free(arr);

    /* regression: never read past `len` (input need not be NUL-terminated) */
    rbl_jv *n3 = rbl_jv_parse("123456", 3);
    CHECK(n3 && n3->type == RBL_JV_NUM && n3->num == 123.0);
    rbl_jv_free(n3);
    CHECK(rbl_jv_parse("\"ab\"cd\"", 3) == NULL); /* unterminated within len */
    rbl_jv *s4 = rbl_jv_parse("\"ab\"cd\"", 4);
    CHECK(s4 && s4->type == RBL_JV_STR);
    if (s4) CHECK_STREQ(s4->str, "ab");
    rbl_jv_free(s4);

    /* embedded NUL ends the document -> rejected as trailing data */
    CHECK(rbl_jv_parse("{}\0{}", 5) == NULL);

    /* nesting is capped (no stack exhaustion on hostile input) */
    {
        char deep[400];
        for (int i = 0; i < 200; i++) deep[i] = '[';
        for (int i = 200; i < 400; i++) deep[i] = ']';
        CHECK(rbl_jv_parse(deep, sizeof(deep)) == NULL);
    }

    /* regression (found by fuzzing): truncated surrogate pair must not read
     * past the end of the input */
    CHECK(JP("\"\\ud83d\\u") == NULL);
    CHECK(JP("\"\\ud83d\\") == NULL);
    CHECK(JP("\"\\u12") == NULL);
    CHECK(JP("\"\\uZZZZ\"") == NULL); /* non-hex rejected */
    rbl_jv *emoji = JP("\"\\ud83d\\ude00\"");
    CHECK(emoji && emoji->str && strcmp(emoji->str, "\xF0\x9F\x98\x80") == 0);
    rbl_jv_free(emoji);
    rbl_jv *lone = JP("\"\\ud83dx\""); /* unpaired -> U+FFFD */
    CHECK(lone && lone->str && strcmp(lone->str, "\xEF\xBF\xBDx") == 0);
    rbl_jv_free(lone);

    /* unicode escapes decode to UTF-8 */
    const char *ujs = "\"Gr\\u00fc\\u00dfe\"";
    rbl_jv *u = rbl_jv_parse(ujs, strlen(ujs));
    CHECK(u && u->type == RBL_JV_STR);
    if (u) CHECK_STREQ(u->str, "Grüße");
    rbl_jv_free(u);
}
