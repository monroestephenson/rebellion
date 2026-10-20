#include "test.h"
#include "html.h"

void test_html(void) {
    const char *src = "<!DOCTYPE html><html lang=\"de\"><head><title>Hallo</title>"
                      "<style>.x{color:red}</style></head>"
                      "<body><p>Erster <b>Absatz</b><p>Zweiter"
                      "<ul><li>eins<li>zwei</ul>"
                      "<script>var x = '<p>not a tag</p>';</script>"
                      "<a href=\"/pfad\">Link</a></body></html>";

    rbl_node *root = rbl_html_parse(src, strlen(src));
    CHECK(root != NULL);
    if (!root) return;

    /* html lang attr survives */
    /* find <html> */
    rbl_node *html = root->first_child;
    while (html && !rbl_node_is(html, "html")) html = html->next;
    CHECK(html != NULL);
    if (html) CHECK_STREQ(rbl_node_attr(html, "lang"), "de");

    /* <script> content must not have created a <p> element: count <p> */
    /* crude recursive count */
    int pcount = 0;
    rbl_node *stack[256];
    int sp = 0;
    stack[sp++] = root;
    while (sp) {
        rbl_node *n = stack[--sp];
        if (rbl_node_is(n, "p")) pcount++;
        for (rbl_node *c = n->first_child; c && sp < 256; c = c->next) stack[sp++] = c;
    }
    CHECK(pcount == 2);

    rbl_node_free(root);
}
