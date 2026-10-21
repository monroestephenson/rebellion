#include "extract.h"
#include "markdown.h"
#include "url.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/* ---- small tree helpers --------------------------------------------- */

static rbl_node *find_first_tag(rbl_node *n, const char *tag) {
    if (!n) return NULL;
    if (rbl_node_is(n, tag)) return n;
    for (rbl_node *c = n->first_child; c; c = c->next) {
        rbl_node *r = find_first_tag(c, tag);
        if (r) return r;
    }
    return NULL;
}

/* Concatenate descendant text into buf. */
static void gather_text(rbl_node *n, rbl_buf *b) {
    if (!n) return;
    if (n->type == RBL_NODE_TEXT) {
        rbl_buf_append_str(b, n->text);
        return;
    }
    for (rbl_node *c = n->first_child; c; c = c->next) gather_text(c, b);
}

static char *node_text_trimmed(rbl_node *n) {
    rbl_buf b;
    rbl_buf_init(&b);
    gather_text(n, &b);
    char *s = rbl_buf_steal(&b);
    if (!s) return NULL;
    rbl_collapse_ws(s);
    char *t = rbl_trim(s);
    char *out = rbl_strdup(t);
    free(s);
    return out;
}

/* Find a <meta> whose `attr` equals `key` (ci) and return its content. */
static const char *meta_content(rbl_node *root, const char *attr, const char *key) {
    if (!root) return NULL;
    if (rbl_node_is(root, "meta")) {
        const char *a = rbl_node_attr(root, attr);
        if (a && rbl_streq_ci(a, key)) {
            const char *c = rbl_node_attr(root, "content");
            if (c && *c) return c;
        }
    }
    for (rbl_node *c = root->first_child; c; c = c->next) {
        const char *r = meta_content(c, attr, key);
        if (r) return r;
    }
    return NULL;
}

/* ---- metadata ------------------------------------------------------- */

void rbl_extract_meta(rbl_node *root, rbl_meta *out) {
    memset(out, 0, sizeof(*out));

    const char *og = meta_content(root, "property", "og:title");
    rbl_node *titlen = find_first_tag(root, "title");
    if (titlen) out->title = node_text_trimmed(titlen);
    if ((!out->title || !*out->title) && og) {
        free(out->title);
        out->title = rbl_strdup(og);
    }
    if ((!out->title || !*out->title)) {
        rbl_node *h1 = find_first_tag(root, "h1");
        if (h1) {
            free(out->title);
            out->title = node_text_trimmed(h1);
        }
    }

    const char *desc = meta_content(root, "name", "description");
    if (!desc) desc = meta_content(root, "property", "og:description");
    if (desc) out->description = rbl_strdup(desc);

    rbl_node *html = find_first_tag(root, "html");
    const char *lang = html ? rbl_node_attr(html, "lang") : NULL;
    if (lang && *lang) out->language = rbl_strdup(lang);
}

void rbl_meta_free(rbl_meta *m) {
    if (!m) return;
    free(m->title);
    free(m->description);
    free(m->language);
    memset(m, 0, sizeof(*m));
}

/* ---- links ---------------------------------------------------------- */

typedef struct {
    rbl_strlist *list;
    rbl_strset seen;
    const char *base;
} link_ctx;

static void collect_links_rec(rbl_node *n, link_ctx *lc) {
    if (!n) return;
    if (rbl_node_is(n, "a")) {
        const char *href = rbl_node_attr(n, "href");
        if (href && *href && href[0] != '#' &&
            !rbl_strcaseprefix(href, "javascript:") &&
            !rbl_strcaseprefix(href, "mailto:") && !rbl_strcaseprefix(href, "tel:") &&
            !rbl_strcaseprefix(href, "data:")) {
            char *abs = rbl_url_resolve(lc->base, href);
            if (abs) {
                char *clean = rbl_url_strip_fragment(abs);
                free(abs);
                if (clean && rbl_strset_add(&lc->seen, clean)) {
                    char **items = realloc(lc->list->items,
                                           (lc->list->count + 1) * sizeof(char *));
                    if (items) {
                        lc->list->items = items;
                        lc->list->items[lc->list->count++] = clean;
                        clean = NULL;
                    }
                }
                free(clean);
            }
        }
    }
    for (rbl_node *c = n->first_child; c; c = c->next) collect_links_rec(c, lc);
}

void rbl_collect_links(rbl_node *root, const char *base_url, rbl_strlist *out) {
    link_ctx lc;
    lc.list = out;
    lc.base = base_url;
    rbl_strset_init(&lc.seen);
    /* Pre-seed the set with anything already in the list. */
    for (size_t i = 0; i < out->count; i++) rbl_strset_add(&lc.seen, out->items[i]);
    collect_links_rec(root, &lc);
    rbl_strset_free(&lc.seen);
}

/* ---- main content heuristic ----------------------------------------- */

/* Count text length and link-text length under a node (one pass). */
static void density(rbl_node *n, size_t *text, size_t *linktext, int in_link) {
    if (!n) return;
    if (n->type == RBL_NODE_TEXT) {
        size_t l = n->text ? strlen(n->text) : 0;
        *text += l;
        if (in_link) *linktext += l;
        return;
    }
    if (rbl_node_is(n, "a")) in_link = 1;
    for (rbl_node *c = n->first_child; c; c = c->next)
        density(c, text, linktext, in_link);
}

static bool is_boilerplate(const rbl_node *n) {
    static const char *const v[] = {"aside", "script", "style",  "noscript", "form",
                                    "svg",   "button", "iframe", NULL};
    for (int i = 0; v[i]; i++)
        if (strcmp(n->tag, v[i]) == 0) return true;
    return rbl_node_is_page_chrome(n);
}

static long score_node(rbl_node *n) {
    size_t text = 0, linktext = 0;
    density(n, &text, &linktext, 0);
    if (text < 200) return -1;
    /* Penalize link-heavy blocks (menus, link farms). */
    double link_ratio = text ? (double)linktext / (double)text : 0.0;
    long score = (long)text;
    score -= (long)(link_ratio * (double)text * 1.5);
    return score;
}

static rbl_node *best_block(rbl_node *n, long *best_score, rbl_node *best) {
    if (!n) return best;
    if (n->type == RBL_NODE_ELEMENT && n->tag) {
        if (is_boilerplate(n)) return best; /* don't descend */
        static const char *const containers[] = {"article", "main", "section",
                                                 "div",     "td",   NULL};
        bool is_container = false;
        for (int i = 0; containers[i]; i++)
            if (strcmp(n->tag, containers[i]) == 0) {
                is_container = true;
                break;
            }
        const char *role = rbl_node_attr(n, "role");
        if (role && rbl_streq_ci(role, "main")) is_container = true;
        if (is_container) {
            long s = score_node(n);
            /* Prefer semantic tags with a bonus. */
            if (rbl_node_is(n, "article"))
                s += 300;
            else if (rbl_node_is(n, "main"))
                s += 250;
            if (s > *best_score) {
                *best_score = s;
                best = n;
            }
        }
    }
    for (rbl_node *c = n->first_child; c; c = c->next)
        best = best_block(c, best_score, best);
    return best;
}

rbl_node *rbl_find_main_content(rbl_node *root) {
    /* A single <article> or <main> wins outright. */
    rbl_node *article = find_first_tag(root, "article");
    rbl_node *main_el = find_first_tag(root, "main");

    long best_score = -1;
    rbl_node *best = best_block(root, &best_score, NULL);

    if (main_el && best != main_el) {
        long s = score_node(main_el);
        if (s + 250 >= best_score) best = main_el;
    }
    if (article && best != article) {
        long s = score_node(article);
        if (s + 300 >= best_score) best = article;
    }

    if (best) return best;
    rbl_node *body = find_first_tag(root, "body");
    return body ? body : root;
}

/* ---- the page's headline -------------------------------------------- */

static bool is_ancestor_or_self(const rbl_node *a, const rbl_node *n) {
    for (; n; n = n->parent)
        if (n == a) return true;
    return false;
}

static bool lead_skip(const rbl_node *n) {
    static const char *const v[] = {"script", "style", "noscript", "template", NULL};
    for (int i = 0; v[i]; i++)
        if (rbl_node_is(n, v[i])) return true;
    const char *ah = rbl_node_attr(n, "aria-hidden");
    return rbl_node_attr(n, "hidden") || (ah && rbl_streq_ci(ah, "true"));
}

/* The first <h1> with real text that is not just a linked logo. Page chrome
 * is skipped unless `in_chrome` (a hero <header> can sit at page level). */
static rbl_node *first_h1(rbl_node *n, bool in_chrome) {
    if (!n) return NULL;
    if (n->type == RBL_NODE_ELEMENT && n->tag) {
        if (lead_skip(n) || rbl_node_is(n, "nav")) return NULL;
        if (!in_chrome && rbl_node_is_page_chrome(n)) return NULL;
        if (rbl_node_is(n, "h1")) {
            size_t text = 0, linktext = 0;
            density(n, &text, &linktext, 0);
            return (text > 0 && linktext * 2 < text) ? n : NULL;
        }
    }
    for (rbl_node *c = n->first_child; c; c = c->next) {
        rbl_node *r = first_h1(c, in_chrome);
        if (r) return r;
    }
    return NULL;
}

/* -1 if `a` comes first in document order, 1 if `b` does, 0 if neither. */
static int doc_order(const rbl_node *n, const rbl_node *a, const rbl_node *b) {
    if (n == a) return -1;
    if (n == b) return 1;
    for (const rbl_node *c = n->first_child; c; c = c->next) {
        int r = doc_order(c, a, b);
        if (r) return r;
    }
    return 0;
}

rbl_node *rbl_find_lead(rbl_node *root, rbl_node *content) {
    if (!root || !content || content == root) return NULL;
    rbl_node *h1 = first_h1(root, false);
    if (!h1) h1 = first_h1(root, true);
    if (!h1 || is_ancestor_or_self(content, h1) || is_ancestor_or_self(h1, content))
        return NULL;
    if (doc_order(root, h1, content) != -1) return NULL; /* headline comes first */

    /* Widen to the block around the headline (its intro, the offer line) up
     * to where it meets the content's branch, unless that block is mostly
     * links, e.g. a site header with a menu. */
    rbl_node *block = h1;
    while (block->parent && !is_ancestor_or_self(block->parent, content))
        block = block->parent;
    size_t text = 0, linktext = 0;
    density(block, &text, &linktext, 0);
    return (text <= 3000 && linktext * 2 <= text) ? block : h1;
}

char *rbl_main_content_markdown(rbl_node *root, const char *base_url) {
    rbl_node *content = rbl_find_main_content(root);
    char *md = rbl_render_markdown(content, base_url, true);
    rbl_node *lead = rbl_find_lead(root, content);
    if (!md || !lead) return md;
    char *head = rbl_render_markdown(lead, base_url, true);
    if (!head) return md;
    rbl_buf b;
    rbl_buf_init(&b);
    rbl_buf_append_str(&b, head);
    rbl_buf_append_str(&b, "\n");
    rbl_buf_append_str(&b, md);
    free(head);
    free(md);
    return rbl_buf_steal(&b);
}
