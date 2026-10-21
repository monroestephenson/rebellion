/* Content extraction over a parsed HTML tree. */
#ifndef REBELLION_EXTRACT_H
#define REBELLION_EXTRACT_H

#include "html.h"
#include "util.h"
#include "rebellion/rebellion.h"

typedef struct {
    char *title;
    char *description;
    char *language;
} rbl_meta;

/* Extract <title>/description/lang. Fields are heap strings or NULL. */
void rbl_extract_meta(rbl_node *root, rbl_meta *out);
void rbl_meta_free(rbl_meta *m);

/* Collect absolute http(s) links (deduped, fragments stripped) found under
 * `root`, resolving relative hrefs against base_url. Appends to `out`. */
void rbl_collect_links(rbl_node *root, const char *base_url, rbl_strlist *out);

/* Pick the best main-content container (article/main/high-density block). May
 * return `root` itself if nothing better is found. Never NULL for a valid
 * tree. The returned node is owned by the tree (do not free). */
rbl_node *rbl_find_main_content(rbl_node *root);

/* The block holding the page's headline (<h1>) when `content` does not
 * contain it — e.g. a hero section next to the text block — or NULL. */
rbl_node *rbl_find_lead(rbl_node *root, rbl_node *content);

/* Markdown for the page's main content: the main block, preceded by the
 * headline block when the main block lacks it. Page chrome is dropped.
 * Heap string (caller frees), or NULL on OOM. */
char *rbl_main_content_markdown(rbl_node *root, const char *base_url);

#endif /* REBELLION_EXTRACT_H */
