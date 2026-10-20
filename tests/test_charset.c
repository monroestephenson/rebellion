#include "test.h"
#include "charset.h"

void test_charset(void) {
    char *cs;
    size_t n;

    cs = rbl_charset_from_content_type("text/html; charset=ISO-8859-1");
    CHECK_STREQ(cs, "iso-8859-1");
    free(cs);

    cs = rbl_charset_from_content_type("text/html; charset=\"UTF-8\"");
    CHECK_STREQ(cs, "utf-8");
    free(cs);

    cs = rbl_charset_from_content_type("text/plain");
    CHECK(cs == NULL);
    free(cs);

    const char *html = "<html><head><meta charset=\"iso-8859-15\"></head></html>";
    cs = rbl_charset_from_html(html, strlen(html));
    CHECK_STREQ(cs, "iso-8859-15");
    free(cs);

    const char *html2 =
        "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=windows-1252\">";
    cs = rbl_charset_from_html(html2, strlen(html2));
    CHECK_STREQ(cs, "windows-1252");
    free(cs);

    /* Latin-1 'ü' (0xFC) and 'ß' (0xDF) -> UTF-8 */
    const char latin1[] = { 'G','r', (char)0xFC, (char)0xDF, 'e', 0 };
    char *utf = rbl_to_utf8(latin1, 5, "iso-8859-1", &n);
    CHECK_STREQ(utf, "Grüße");
    free(utf);

    /* already-UTF-8 passes through unchanged */
    const char *u8 = "Grüße";
    utf = rbl_to_utf8(u8, strlen(u8), "utf-8", &n);
    CHECK_STREQ(utf, "Grüße");
    free(utf);

    /* undeclared, invalid-utf8 bytes assumed latin1 */
    const char win[] = { 'c','a','f', (char)0xE9, 0 }; /* café in latin1 */
    utf = rbl_to_utf8(win, 4, NULL, &n);
    CHECK_STREQ(utf, "café");
    free(utf);
}
