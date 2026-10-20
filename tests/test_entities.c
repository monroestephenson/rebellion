#include "test.h"
#include "entities.h"

void test_entities(void) {
    char *s;

    s = rbl_decode_entities_str("Gr&uuml;&szlig;e &amp; Tsch&uuml;ss");
    CHECK_STREQ(s, "Grüße & Tschüss"); /* Grüße & Tschüss */
    free(s);

    s = rbl_decode_entities_str("&lt;tag&gt; &quot;x&quot;");
    CHECK_STREQ(s, "<tag> \"x\"");
    free(s);

    /* numeric + hex + euro */
    s = rbl_decode_entities_str("&#8364; &#x20AC; &#228;");
    CHECK_STREQ(s, "€ € ä");
    free(s);

    /* windows-1252 numeric remap (0x92 -> right single quote) */
    s = rbl_decode_entities_str("it&#146;s");
    CHECK_STREQ(s, "it’s");
    free(s);

    /* unknown entity passes through literally */
    s = rbl_decode_entities_str("a &notareal; b");
    CHECK_CONTAINS(s, "&notareal;");
    free(s);
}
