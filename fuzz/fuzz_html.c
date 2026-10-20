/* libFuzzer entry point: exercise the HTML parser, content extraction and
 * Markdown renderer on arbitrary input — the untrusted-data attack surface. */
#include "html.h"
#include "markdown.h"
#include "extract.h"

#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    rbl_node *root = rbl_html_parse((const char *)data, size);
    if (!root) return 0;

    rbl_meta meta;
    rbl_extract_meta(root, &meta);
    rbl_meta_free(&meta);

    rbl_node *content = rbl_find_main_content(root);
    char *md = rbl_render_markdown(content, "https://example.com/base/", true);
    free(md);

    char *md2 = rbl_render_markdown(root, "https://example.com/base/", false);
    free(md2);

    rbl_node_free(root);
    return 0;
}
