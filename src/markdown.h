/* Render a parsed HTML subtree to clean GitHub-flavored Markdown. */
#ifndef REBELLION_MARKDOWN_H
#define REBELLION_MARKDOWN_H

#include "html.h"

/* Render `content_root` and its descendants to Markdown. Links/images are
 * resolved against base_url. When drop_boilerplate is true, navigational
 * chrome (nav/header/footer/aside/forms/buttons) encountered inside the tree
 * is skipped. Returns a heap string (caller frees); NULL only on OOM. */
char *rbl_render_markdown(rbl_node *content_root, const char *base_url,
                          bool drop_boilerplate);

#endif /* REBELLION_MARKDOWN_H */
