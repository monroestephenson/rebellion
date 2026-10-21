/* libFuzzer entry point: exercise charset detection/conversion, the HTML
 * parser, content extraction, link resolution and the Markdown renderer on
 * arbitrary input — the untrusted-data attack surface. */
#include "charset.h"
#include "html.h"
#include "markdown.h"
#include "extract.h"
#include "rebellion/rebellion.h"

#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* What the HTTP engine does with a body before parsing it. */
    char *cs = rbl_charset_from_html((const char *)data, size);
    size_t ulen = 0;
    char *utf8 = rbl_to_utf8((const char *)data, size, cs, &ulen);
    free(cs);
    free(utf8);

    rbl_node *root = rbl_html_parse((const char *)data, size);
    if (!root) return 0;

    rbl_strlist links = {0};
    rbl_collect_links(root, "https://example.com/base/page.html", &links);
    rbl_strlist_free(&links);

    rbl_meta meta;
    rbl_extract_meta(root, &meta);
    rbl_meta_free(&meta);

    rbl_node *content = rbl_find_main_content(root);
    char *md = rbl_render_markdown(content, "https://example.com/base/", true);
    free(md);

    char *md2 = rbl_render_markdown(root, "https://example.com/base/", false);
    free(md2);

    char *md3 = rbl_main_content_markdown(root, "https://example.com/base/");
    free(md3);

    rbl_node_free(root);
    return 0;
}
