/* A pragmatic HTML parser: tokenizer + a forgiving tree builder.
 *
 * This is not a full HTML5 tree-construction implementation, but it handles
 * real-world markup — unclosed <p>/<li>, raw <script>/<style>, void elements,
 * comments, entities — well enough to extract content and emit Markdown. */
#ifndef REBELLION_HTML_H
#define REBELLION_HTML_H

#include <stddef.h>
#include <stdbool.h>

typedef enum { RBL_NODE_DOCUMENT, RBL_NODE_ELEMENT, RBL_NODE_TEXT } rbl_node_type;

typedef struct rbl_attr {
    char *name;  /* lowercased */
    char *value; /* entity-decoded UTF-8, may be NULL for valueless attrs */
    struct rbl_attr *next;
} rbl_attr;

typedef struct rbl_node {
    rbl_node_type type;
    char *tag;  /* lowercased tag name (elements only) */
    char *text; /* entity-decoded UTF-8 (text nodes only) */
    rbl_attr *attrs;
    struct rbl_node *parent;
    struct rbl_node *first_child, *last_child;
    struct rbl_node *next, *prev;
} rbl_node;

/* Parse UTF-8 HTML into a document tree. Never returns NULL for valid input;
 * returns NULL only on OOM. Free with rbl_node_free on the returned root. */
rbl_node *rbl_html_parse(const char *html, size_t len);

void rbl_node_free(rbl_node *root);

/* Attribute lookup (case-insensitive name). Returns value or NULL. */
const char *rbl_node_attr(const rbl_node *n, const char *name);

/* True if the element tag equals `tag` (case-insensitive). */
bool rbl_node_is(const rbl_node *n, const char *tag);

/* True for page-level chrome: <nav>, role=navigation/banner/contentinfo, and
 * a <header>/<footer> that belongs to the page rather than to a piece of
 * content. Inside <main>, <article>, <section>, <aside> or <nav> a header or
 * footer introduces or annotates that content (a product's headline, an
 * offer's footnotes) and is not chrome — the rule ARIA uses for the banner
 * and contentinfo landmarks. */
bool rbl_node_is_page_chrome(const rbl_node *n);

#endif /* REBELLION_HTML_H */
