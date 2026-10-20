#include "html.h"
#include "entities.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define RBL_MAX_DEPTH 400

/* ---- tag classification --------------------------------------------- */

static bool tag_in(const char *t, const char *const *set) {
    for (int i = 0; set[i]; i++)
        if (strcmp(t, set[i]) == 0) return true;
    return false;
}

static bool is_void(const char *t) {
    static const char *const v[] = {"area",  "base",   "br",    "col",  "embed",
                                    "hr",    "img",    "input", "link", "meta",
                                    "param", "source", "track", "wbr",  NULL};
    return tag_in(t, v);
}

/* Elements whose content is raw text up to the matching close tag. */
static bool is_rawtext(const char *t) {
    static const char *const v[] = {"script", "style",    "xmp",
                                    "iframe", "noscript", NULL};
    return tag_in(t, v);
}
/* Escapable raw text (entities decoded) up to the matching close tag. */
static bool is_escapable_raw(const char *t) {
    static const char *const v[] = {"textarea", "title", NULL};
    return tag_in(t, v);
}

static bool is_block_for_p_close(const char *t) {
    static const char *const v[] = {
        "address", "article",  "aside",      "blockquote", "details", "div",
        "dl",      "fieldset", "figcaption", "figure",     "footer",  "form",
        "h1",      "h2",       "h3",         "h4",         "h5",      "h6",
        "header",  "hgroup",   "hr",         "main",       "menu",    "nav",
        "ol",      "p",        "pre",        "section",    "table",   "ul",
        NULL};
    return tag_in(t, v);
}

/* ---- node construction ---------------------------------------------- */

static rbl_node *node_new(rbl_node_type type) {
    rbl_node *n = (rbl_node *)calloc(1, sizeof(*n));
    if (n) n->type = type;
    return n;
}

static void node_append_child(rbl_node *parent, rbl_node *child) {
    child->parent = parent;
    child->prev = parent->last_child;
    child->next = NULL;
    if (parent->last_child)
        parent->last_child->next = child;
    else
        parent->first_child = child;
    parent->last_child = child;
}

static void attr_free(rbl_attr *a) {
    while (a) {
        rbl_attr *n = a->next;
        free(a->name);
        free(a->value);
        free(a);
        a = n;
    }
}

void rbl_node_free(rbl_node *n) {
    if (!n) return;
    rbl_node *c = n->first_child;
    while (c) {
        rbl_node *nx = c->next;
        rbl_node_free(c);
        c = nx;
    }
    free(n->tag);
    free(n->text);
    attr_free(n->attrs);
    free(n);
}

const char *rbl_node_attr(const rbl_node *n, const char *name) {
    if (!n) return NULL;
    for (rbl_attr *a = n->attrs; a; a = a->next)
        if (rbl_streq_ci(a->name, name)) return a->value;
    return NULL;
}

bool rbl_node_is(const rbl_node *n, const char *tag) {
    return n && n->type == RBL_NODE_ELEMENT && n->tag && rbl_streq_ci(n->tag, tag);
}

/* ---- tokenizer / tree builder --------------------------------------- */

typedef struct {
    const char *p;
    const char *end;
    rbl_node *stack[RBL_MAX_DEPTH];
    int depth;
    rbl_node *root;
} parser;

static rbl_node *cur(parser *ps) {
    return ps->stack[ps->depth - 1];
}

static bool push(parser *ps, rbl_node *n) {
    if (ps->depth >= RBL_MAX_DEPTH) return false;
    ps->stack[ps->depth++] = n;
    return true;
}
static void pop(parser *ps) {
    if (ps->depth > 1) ps->depth--;
}

/* Pop until (and including) the nearest open element with this tag. */
static void pop_to_tag(parser *ps, const char *tag) {
    for (int i = ps->depth - 1; i >= 1; i--) {
        if (ps->stack[i]->tag && strcmp(ps->stack[i]->tag, tag) == 0) {
            ps->depth = i; /* drop i..top */
            return;
        }
    }
    /* No matching open element: ignore the stray end tag. */
}

static void emit_text(parser *ps, const char *start, size_t len, bool decode) {
    if (len == 0) return;
    rbl_node *t = node_new(RBL_NODE_TEXT);
    if (!t) return;
    if (decode) {
        rbl_buf b;
        rbl_buf_init(&b);
        rbl_decode_entities(&b, start, len);
        t->text = rbl_buf_steal(&b);
    } else {
        t->text = rbl_strndup(start, len);
    }
    node_append_child(cur(ps), t);
}

/* Apply implicit-close rules before opening `tag`. */
static void implicit_close(parser *ps, const char *tag) {
    const char *c = cur(ps)->tag;
    if (!c) return;
    if (strcmp(c, "p") == 0 && is_block_for_p_close(tag)) {
        pop(ps);
        return;
    }
    if (strcmp(tag, "li") == 0 && strcmp(c, "li") == 0) {
        pop(ps);
        return;
    }
    if ((strcmp(tag, "dt") == 0 || strcmp(tag, "dd") == 0) &&
        (strcmp(c, "dt") == 0 || strcmp(c, "dd") == 0)) {
        pop(ps);
        return;
    }
    if (strcmp(tag, "option") == 0 && strcmp(c, "option") == 0) {
        pop(ps);
        return;
    }
    if (strcmp(tag, "tr") == 0) {
        if (strcmp(c, "td") == 0 || strcmp(c, "th") == 0) {
            pop(ps);
            c = cur(ps)->tag;
        }
        if (c && strcmp(c, "tr") == 0) pop(ps);
        return;
    }
    if ((strcmp(tag, "td") == 0 || strcmp(tag, "th") == 0) &&
        (strcmp(c, "td") == 0 || strcmp(c, "th") == 0)) {
        pop(ps);
        return;
    }
    if ((strcmp(tag, "thead") == 0 || strcmp(tag, "tbody") == 0 ||
         strcmp(tag, "tfoot") == 0)) {
        if (strcmp(c, "td") == 0 || strcmp(c, "th") == 0) pop(ps);
        c = cur(ps)->tag;
        if (c && strcmp(c, "tr") == 0) pop(ps);
        return;
    }
}

static char *read_name(const char **pp, const char *end) {
    const char *p = *pp;
    const char *start = p;
    while (p < end &&
           (isalnum((unsigned char)*p) || *p == '-' || *p == ':' || *p == '_'))
        p++;
    if (p == start) {
        *pp = p;
        return NULL;
    }
    char *name = rbl_strndup(start, (size_t)(p - start));
    rbl_str_tolower_inplace(name);
    *pp = p;
    return name;
}

/* Parse attributes until '>' or end. Advances *pp past the closing '>'.
 * Sets *self_closing if the tag ends with "/>". */
static rbl_attr *parse_attrs(const char **pp, const char *end, bool *self_closing) {
    const char *p = *pp;
    rbl_attr *head = NULL, *tail = NULL;
    *self_closing = false;

    for (;;) {
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end) break;
        if (*p == '>') {
            p++;
            break;
        }
        if (*p == '/') {
            p++;
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p < end && *p == '>') {
                *self_closing = true;
                p++;
            } else
                continue;
            break;
        }
        /* attribute name */
        const char *ns = p;
        while (p < end && *p != '=' && *p != '>' && *p != '/' &&
               !isspace((unsigned char)*p))
            p++;
        if (p == ns) {
            p++;
            continue;
        } /* skip stray char */
        char *aname = rbl_strndup(ns, (size_t)(p - ns));
        rbl_str_tolower_inplace(aname);

        char *aval = NULL;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p < end && *p == '=') {
            p++;
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p < end && (*p == '"' || *p == '\'')) {
                char q = *p++;
                const char *vs = p;
                while (p < end && *p != q) p++;
                aval = rbl_decode_entities_str_n(vs, (size_t)(p - vs));
                if (p < end) p++; /* skip closing quote */
            } else {
                const char *vs = p;
                while (p < end && *p != '>' && !isspace((unsigned char)*p)) p++;
                aval = rbl_decode_entities_str_n(vs, (size_t)(p - vs));
            }
        }

        rbl_attr *a = (rbl_attr *)calloc(1, sizeof(*a));
        if (a) {
            a->name = aname;
            a->value = aval;
            if (tail)
                tail->next = a;
            else
                head = a;
            tail = a;
        } else {
            free(aname);
            free(aval);
        }
    }
    *pp = p;
    return head;
}

/* Consume raw text until the matching </tag>, emitting one text node. */
static void consume_rawtext(parser *ps, const char *tag, bool decode) {
    const char *p = ps->p, *end = ps->end;
    size_t tlen = strlen(tag);
    const char *text_start = p;
    while (p < end) {
        if (*p == '<' && p + 1 < end && p[1] == '/') {
            const char *q = p + 2;
            if ((size_t)(end - q) >= tlen && strncasecmp(q, tag, tlen) == 0) {
                const char *after = q + tlen;
                if (after >= end || *after == '>' || isspace((unsigned char)*after)) {
                    emit_text(ps, text_start, (size_t)(p - text_start), decode);
                    /* advance past </tag ...> */
                    while (p < end && *p != '>') p++;
                    if (p < end) p++;
                    ps->p = p;
                    return;
                }
            }
        }
        p++;
    }
    emit_text(ps, text_start, (size_t)(p - text_start), decode);
    ps->p = p;
}

rbl_node *rbl_html_parse(const char *html, size_t len) {
    parser ps;
    ps.p = html ? html : "";
    ps.end = ps.p + len;
    ps.depth = 0;
    ps.root = node_new(RBL_NODE_DOCUMENT);
    if (!ps.root) return NULL;
    push(&ps, ps.root);

    while (ps.p < ps.end) {
        if (*ps.p != '<') {
            const char *ts = ps.p;
            while (ps.p < ps.end && *ps.p != '<') ps.p++;
            emit_text(&ps, ts, (size_t)(ps.p - ts), true);
            continue;
        }
        /* We are at '<'. */
        const char *p = ps.p + 1;
        if (p >= ps.end) {
            emit_text(&ps, ps.p, 1, true);
            break;
        }

        if (*p == '!') {
            if ((size_t)(ps.end - p) >= 3 && p[1] == '-' && p[2] == '-') {
                /* comment */
                const char *q = p + 3;
                while (q + 2 < ps.end && !(q[0] == '-' && q[1] == '-' && q[2] == '>'))
                    q++;
                ps.p = (q + 2 < ps.end) ? q + 3 : ps.end;
            } else {
                /* doctype / declaration */
                while (p < ps.end && *p != '>') p++;
                ps.p = (p < ps.end) ? p + 1 : ps.end;
            }
            continue;
        }
        if (*p == '?') { /* processing instruction */
            while (p < ps.end && *p != '>') p++;
            ps.p = (p < ps.end) ? p + 1 : ps.end;
            continue;
        }
        if (*p == '/') {
            p++;
            char *name = read_name(&p, ps.end);
            while (p < ps.end && *p != '>') p++;
            if (p < ps.end) p++;
            ps.p = p;
            if (name) {
                pop_to_tag(&ps, name);
                free(name);
            }
            continue;
        }
        if (!isalpha((unsigned char)*p)) {
            /* Lone '<' not starting a tag: literal text. */
            emit_text(&ps, ps.p, 1, true);
            ps.p++;
            continue;
        }

        /* start tag */
        char *name = read_name(&p, ps.end);
        if (!name) {
            emit_text(&ps, ps.p, 1, true);
            ps.p++;
            continue;
        }

        bool self_closing = false;
        rbl_attr *attrs = parse_attrs(&p, ps.end, &self_closing);
        ps.p = p;

        implicit_close(&ps, name);

        rbl_node *el = node_new(RBL_NODE_ELEMENT);
        if (!el) {
            free(name);
            attr_free(attrs);
            continue;
        }
        el->tag = name;
        el->attrs = attrs;
        node_append_child(cur(&ps), el);

        if (is_void(name) || self_closing) {
            /* no children */
        } else if (is_rawtext(name)) {
            if (!push(&ps, el)) { /* too deep */
            }
            consume_rawtext(&ps, name, false);
            pop(&ps);
        } else if (is_escapable_raw(name)) {
            if (!push(&ps, el)) {
            }
            consume_rawtext(&ps, name, true);
            pop(&ps);
        } else {
            if (!push(&ps, el)) {
                /* Depth exceeded: keep node but don't descend. */
            }
        }
    }

    return ps.root;
}
