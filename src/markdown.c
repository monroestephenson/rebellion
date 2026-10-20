#include "markdown.h"
#include "url.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define MAX_LIST 24

typedef struct {
    rbl_buf out;
    rbl_buf prefix; /* continuation prefix for new lines */
    char *oneshot;  /* used once instead of prefix at next line start */
    bool at_bol;    /* at beginning of a line */
    bool want_gap;  /* blank line requested before next content */
    bool pending_space;
    bool in_pre;
    const char *base;
    bool drop_boiler;
    int list_depth;
    struct {
        bool ordered;
        int counter;
    } lists[MAX_LIST];
} md_ctx;

static void md_raw(md_ctx *m, const char *s, size_t n) {
    rbl_buf_append(&m->out, s, n);
    if (n) m->at_bol = (s[n - 1] == '\n');
}
static void md_raws(md_ctx *m, const char *s) {
    md_raw(m, s, strlen(s));
}

/* Right-trimmed copy length of the prefix (for blank lines). */
static void emit_prefix_rtrim(md_ctx *m) {
    const char *p = m->prefix.data ? m->prefix.data : "";
    size_t n = m->prefix.len;
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    md_raw(m, p, n);
}

/* Realize a requested block gap: end the current line and emit one blank
 * line carrying the current prefix. A no-op unless a gap was requested, so
 * inline text flows normally. */
static void flush_gap(md_ctx *m) {
    if (!m->want_gap) return;
    m->want_gap = false;
    if (m->out.len == 0) return;      /* nothing before us */
    if (!m->at_bol) md_raws(m, "\n"); /* end current line */
    emit_prefix_rtrim(m);             /* blank separator line */
    md_raws(m, "\n");
}

static void ensure_line(md_ctx *m) {
    flush_gap(m);
    if (m->at_bol) {
        if (m->oneshot) {
            md_raws(m, m->oneshot);
            free(m->oneshot);
            m->oneshot = NULL;
        } else if (m->prefix.len) {
            md_raw(m, m->prefix.data, m->prefix.len);
        }
        m->at_bol = false;
    }
}

static void md_newline(md_ctx *m) {
    if (!m->at_bol) md_raws(m, "\n");
}

static void request_gap(md_ctx *m) {
    m->pending_space = false;
    m->want_gap = true;
}

/* Append one inline text char, escaping markdown-significant characters. */
static void put_text_char(md_ctx *m, char c) {
    if (c == '\\' || c == '`' || c == '*' || c == '[' || c == ']') {
        char esc[2] = {'\\', c};
        md_raw(m, esc, 2);
    } else {
        md_raw(m, &c, 1);
    }
}

/* Inline text with whitespace collapsing. */
static void md_text(md_ctx *m, const char *s) {
    if (!s) return;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isspace(c)) {
            m->pending_space = true;
            continue;
        }
        if (m->pending_space) {
            m->pending_space = false;
            if (!m->at_bol) md_raws(m, " ");
        }
        ensure_line(m);
        put_text_char(m, (char)c);
    }
}

/* Raw inline markup (markers like ** or link syntax); honors pending space. */
static void md_markup(md_ctx *m, const char *s) {
    if (m->pending_space) {
        m->pending_space = false;
        if (!m->at_bol) md_raws(m, " ");
    }
    ensure_line(m);
    md_raws(m, s);
}

static char *resolve(md_ctx *m, const char *ref) {
    if (!ref || !*ref) return NULL;
    return rbl_url_resolve(m->base, ref);
}

/* ---- element filtering ---------------------------------------------- */

static bool skip_always(const char *t) {
    static const char *const v[] = {
        "head",     "title", "meta", "link",   "script", "style",  "noscript", "svg",
        "template", "base",  "map",  "object", "param",  "source", "track",    NULL};
    for (int i = 0; v[i]; i++)
        if (strcmp(t, v[i]) == 0) return true;
    return false;
}
static bool skip_boiler(const char *t) {
    static const char *const v[] = {"nav",    "header", "footer", "aside", "form",
                                    "button", "iframe", "dialog", "menu",  NULL};
    for (int i = 0; v[i]; i++)
        if (strcmp(t, v[i]) == 0) return true;
    return false;
}

static bool node_hidden(rbl_node *n) {
    if (rbl_node_attr(n, "hidden")) return true;
    const char *ah = rbl_node_attr(n, "aria-hidden");
    if (ah && rbl_streq_ci(ah, "true")) return true;
    const char *st = rbl_node_attr(n, "style");
    if (st && (strstr(st, "display:none") || strstr(st, "display: none"))) return true;
    return false;
}

static void render_node(md_ctx *m, rbl_node *n);
static void render_children(md_ctx *m, rbl_node *n) {
    for (rbl_node *c = n->first_child; c; c = c->next) render_node(m, c);
}

/* Render a subtree to a one-line inline string (for table cells). */
static char *render_inline_string(rbl_node *n, const char *base) {
    md_ctx t;
    memset(&t, 0, sizeof(t));
    rbl_buf_init(&t.out);
    rbl_buf_init(&t.prefix);
    t.at_bol = true;
    t.base = base;
    render_children(&t, n);
    free(t.oneshot);
    char *s = rbl_buf_steal(&t.out);
    rbl_buf_free(&t.prefix);
    if (!s) return rbl_strdup("");
    /* Collapse any newlines to spaces and escape pipes for table cells. */
    rbl_buf b;
    rbl_buf_init(&b);
    for (char *p = s; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            rbl_buf_append_ch(&b, ' ');
        } else if (*p == '|') {
            rbl_buf_append_str(&b, "\\|");
        } else
            rbl_buf_append_ch(&b, *p);
    }
    free(s);
    char *r = rbl_buf_steal(&b);
    rbl_collapse_ws(r);
    char *tr = rbl_trim(r);
    char *out = rbl_strdup(tr);
    free(r);
    return out;
}

/* ---- block helpers -------------------------------------------------- */

static void render_heading(md_ctx *m, rbl_node *n, int level) {
    request_gap(m);
    char hashes[8];
    int i = 0;
    for (; i < level && i < 6; i++) hashes[i] = '#';
    hashes[i] = '\0';
    md_markup(m, hashes);
    md_raws(m, " ");
    m->pending_space = false;
    render_children(m, n);
    request_gap(m);
}

static void render_list(md_ctx *m, rbl_node *n, bool ordered) {
    if (m->list_depth >= MAX_LIST) {
        render_children(m, n);
        return;
    }
    request_gap(m);
    int idx = m->list_depth++;
    m->lists[idx].ordered = ordered;
    int start = 1;
    const char *sa = rbl_node_attr(n, "start");
    if (ordered && sa) {
        int v = atoi(sa);
        if (v > 0) start = v;
    }
    m->lists[idx].counter = start;

    size_t saved_prefix_len = m->prefix.len;
    for (rbl_node *c = n->first_child; c; c = c->next) {
        if (c->type != RBL_NODE_ELEMENT) continue;
        if (!rbl_node_is(c, "li")) continue;

        /* Build marker. */
        char marker[24];
        if (ordered)
            snprintf(marker, sizeof(marker), "%d. ", m->lists[idx].counter++);
        else
            snprintf(marker, sizeof(marker), "- ");
        size_t mw = strlen(marker);

        md_newline(m);       /* finish previous line */
        m->want_gap = false; /* tight list between items */

        /* One-shot lead for the item's first line = prefix + marker. */
        rbl_buf lead;
        rbl_buf_init(&lead);
        rbl_buf_append(&lead, m->prefix.data ? m->prefix.data : "", saved_prefix_len);
        rbl_buf_append_str(&lead, marker);
        free(m->oneshot);
        m->oneshot = rbl_buf_steal(&lead);
        m->at_bol = true;
        m->pending_space = false;

        /* Continuation prefix: indent under the marker. */
        m->prefix.len = saved_prefix_len;
        for (size_t k = 0; k < mw; k++) rbl_buf_append_ch(&m->prefix, ' ');

        render_children(m, c);

        /* If the item produced no output, still emit the marker. */
        if (m->oneshot) {
            ensure_line(m);
        }

        m->prefix.len = saved_prefix_len;
    }
    m->prefix.len = saved_prefix_len;
    m->list_depth--;
    request_gap(m);
}

static void render_table(md_ctx *m, rbl_node *n) {
    request_gap(m);
    md_newline(m);

    int header_cols = -1;

    /* Gather <tr> rows, descending through thead/tbody/tfoot wrappers. */
    rbl_node *rows[1024];
    int nrows = 0;
    rbl_node *dfs[1024];
    int dfsn = 0;
    for (rbl_node *c = n->first_child; c && dfsn < 1024; c = c->next) dfs[dfsn++] = c;
    for (int i = 0; i < dfsn && nrows < 1024; i++) {
        rbl_node *x = dfs[i];
        if (x->type != RBL_NODE_ELEMENT) continue;
        if (rbl_node_is(x, "tr")) {
            rows[nrows++] = x;
            continue;
        }
        if (rbl_node_is(x, "thead") || rbl_node_is(x, "tbody") ||
            rbl_node_is(x, "tfoot")) {
            for (rbl_node *c = x->first_child; c && dfsn < 1024; c = c->next)
                dfs[dfsn++] = c;
        }
    }

    for (int r = 0; r < nrows; r++) {
        rbl_node *tr = rows[r];
        /* cells */
        char *cells[256];
        int ncells = 0;
        bool any_th = false;
        for (rbl_node *c = tr->first_child; c && ncells < 256; c = c->next) {
            if (c->type != RBL_NODE_ELEMENT) continue;
            if (rbl_node_is(c, "td") || rbl_node_is(c, "th")) {
                if (rbl_node_is(c, "th")) any_th = true;
                cells[ncells++] = render_inline_string(c, m->base);
            }
        }
        if (ncells == 0) continue;

        ensure_line(m);
        md_raws(m, "| ");
        for (int i = 0; i < ncells; i++) {
            md_raws(m, cells[i]);
            md_raws(m, " |");
            if (i + 1 < ncells) md_raws(m, " ");
        }
        md_newline(m);

        if (header_cols < 0) {
            /* Emit the separator after the first row (treat it as header). */
            header_cols = ncells;
            ensure_line(m);
            md_raws(m, "|");
            for (int i = 0; i < ncells; i++) md_raws(m, " --- |");
            md_newline(m);
            (void)any_th;
        }
        for (int i = 0; i < ncells; i++) free(cells[i]);
    }
    request_gap(m);
}

static void render_pre(md_ctx *m, rbl_node *n) {
    request_gap(m);
    md_newline(m);
    ensure_line(m);
    md_raws(m, "```");
    md_newline(m);
    /* Gather raw text, preserving whitespace. */
    rbl_buf b;
    rbl_buf_init(&b);
    /* collect all descendant text */
    rbl_node *dfs[4096];
    int dn = 0;
    for (rbl_node *c = n->first_child; c; c = c->next) dfs[dn++] = c;
    for (int i = 0; i < dn && dn < 4096; i++) {
        rbl_node *x = dfs[i];
        if (x->type == RBL_NODE_TEXT)
            rbl_buf_append_str(&b, x->text);
        else
            for (rbl_node *c = x->first_child; c && dn < 4096; c = c->next)
                dfs[dn++] = c;
    }
    char *code = rbl_buf_cstr(&b);
    /* Emit each line with the current prefix. */
    const char *line = code;
    while (*line) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        m->at_bol = true;
        ensure_line(m);
        md_raw(m, line, len);
        md_newline(m);
        if (!nl) break;
        line = nl + 1;
    }
    rbl_buf_free(&b);
    m->at_bol = true;
    ensure_line(m);
    md_raws(m, "```");
    request_gap(m);
}

/* ---- main dispatch -------------------------------------------------- */

static void render_node(md_ctx *m, rbl_node *n) {
    if (n->type == RBL_NODE_TEXT) {
        if (!m->in_pre) md_text(m, n->text);
        return;
    }
    if (n->type == RBL_NODE_DOCUMENT) {
        render_children(m, n);
        return;
    }
    if (n->type != RBL_NODE_ELEMENT || !n->tag) return;

    const char *t = n->tag;
    if (skip_always(t)) return;
    if (m->drop_boiler && skip_boiler(t)) return;
    if (node_hidden(n)) return;

    if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && t[2] == '\0') {
        render_heading(m, n, t[1] - '0');
        return;
    }
    if (!strcmp(t, "p")) {
        request_gap(m);
        render_children(m, n);
        request_gap(m);
        return;
    }
    if (!strcmp(t, "br")) {
        m->pending_space = false;
        md_newline(m);
        return;
    }
    if (!strcmp(t, "hr")) {
        request_gap(m);
        ensure_line(m);
        md_raws(m, "---");
        request_gap(m);
        return;
    }
    if (!strcmp(t, "div") || !strcmp(t, "section") || !strcmp(t, "article") ||
        !strcmp(t, "main") || !strcmp(t, "figure") || !strcmp(t, "figcaption") ||
        !strcmp(t, "details") || !strcmp(t, "summary") || !strcmp(t, "address")) {
        request_gap(m);
        render_children(m, n);
        request_gap(m);
        return;
    }
    if (!strcmp(t, "ul")) {
        render_list(m, n, false);
        return;
    }
    if (!strcmp(t, "ol")) {
        render_list(m, n, true);
        return;
    }
    if (!strcmp(t, "li")) {
        render_children(m, n);
        return;
    } /* handled by list */
    if (!strcmp(t, "dl")) {
        request_gap(m);
        render_children(m, n);
        request_gap(m);
        return;
    }
    if (!strcmp(t, "dt")) {
        request_gap(m);
        md_markup(m, "**");
        m->pending_space = false;
        render_children(m, n);
        md_raws(m, "**");
        request_gap(m);
        return;
    }
    if (!strcmp(t, "dd")) {
        request_gap(m);
        render_children(m, n);
        request_gap(m);
        return;
    }
    if (!strcmp(t, "blockquote")) {
        request_gap(m);
        md_newline(m);
        size_t saved = m->prefix.len;
        rbl_buf_append_str(&m->prefix, "> ");
        m->at_bol = true;
        render_children(m, n);
        m->prefix.len = saved;
        request_gap(m);
        return;
    }
    if (!strcmp(t, "pre")) {
        render_pre(m, n);
        return;
    }
    if (!strcmp(t, "table")) {
        render_table(m, n);
        return;
    }

    if (!strcmp(t, "strong") || !strcmp(t, "b")) {
        md_markup(m, "**");
        m->pending_space = false;
        render_children(m, n);
        md_raws(m, "**");
        return;
    }
    if (!strcmp(t, "em") || !strcmp(t, "i")) {
        md_markup(m, "*");
        m->pending_space = false;
        render_children(m, n);
        md_raws(m, "*");
        return;
    }
    if (!strcmp(t, "code") || !strcmp(t, "kbd") || !strcmp(t, "samp") ||
        !strcmp(t, "tt")) {
        md_markup(m, "`");
        m->pending_space = false;
        bool pv = m->in_pre;
        m->in_pre = true; /* don't escape inside code */
        /* gather text raw */
        rbl_buf b;
        rbl_buf_init(&b);
        rbl_node *dfs[1024];
        int dn = 0;
        for (rbl_node *c = n->first_child; c; c = c->next) dfs[dn++] = c;
        for (int i = 0; i < dn && dn < 1024; i++) {
            rbl_node *x = dfs[i];
            if (x->type == RBL_NODE_TEXT)
                rbl_buf_append_str(&b, x->text);
            else
                for (rbl_node *c = x->first_child; c && dn < 1024; c = c->next)
                    dfs[dn++] = c;
        }
        char *code = rbl_buf_cstr(&b);
        for (char *p = code; p && *p; p++) {
            if (*p == '\n' || *p == '\r') *p = ' ';
        }
        md_raws(m, code ? code : "");
        rbl_buf_free(&b);
        m->in_pre = pv;
        md_raws(m, "`");
        return;
    }
    if (!strcmp(t, "a")) {
        const char *href = rbl_node_attr(n, "href");
        char *abs = NULL;
        if (href && href[0] != '#' && !rbl_strcaseprefix(href, "javascript:"))
            abs = resolve(m, href);
        if (abs) {
            md_markup(m, "[");
            m->pending_space = false;
            render_children(m, n);
            md_raws(m, "](");
            md_raws(m, abs);
            md_raws(m, ")");
            free(abs);
        } else {
            render_children(m, n);
        }
        return;
    }
    if (!strcmp(t, "img")) {
        const char *src = rbl_node_attr(n, "src");
        if (!src || !*src) src = rbl_node_attr(n, "data-src");
        const char *alt = rbl_node_attr(n, "alt");
        char *abs = resolve(m, src);
        if (abs) {
            md_markup(m, "![");
            if (alt) md_raws(m, alt);
            md_raws(m, "](");
            md_raws(m, abs);
            md_raws(m, ")");
            free(abs);
        }
        return;
    }

    /* Generic inline/unknown element: render children in place. */
    render_children(m, n);
}

char *rbl_render_markdown(rbl_node *content_root, const char *base_url,
                          bool drop_boilerplate) {
    md_ctx m;
    memset(&m, 0, sizeof(m));
    rbl_buf_init(&m.out);
    rbl_buf_init(&m.prefix);
    m.at_bol = true;
    m.base = base_url;
    m.drop_boiler = drop_boilerplate;

    render_node(&m, content_root);

    free(m.oneshot);
    rbl_buf_free(&m.prefix);

    /* Final tidy: ensure trailing newline, collapse >2 blank lines. */
    char *raw = rbl_buf_steal(&m.out);
    if (!raw) return NULL;

    rbl_buf out;
    rbl_buf_init(&out);
    int nl_run = 0;
    for (char *p = raw; *p; p++) {
        if (*p == '\n') {
            nl_run++;
            if (nl_run <= 2) rbl_buf_append_ch(&out, '\n');
        } else {
            nl_run = 0;
            rbl_buf_append_ch(&out, *p);
        }
    }
    free(raw);
    /* Trim leading newlines, ensure single trailing newline. */
    char *s = rbl_buf_steal(&out);
    char *start = s;
    while (*start == '\n') start++;
    size_t len = strlen(start);
    while (len > 0 && (start[len - 1] == '\n' || start[len - 1] == ' ')) len--;
    char *final = (char *)malloc(len + 2);
    if (!final) {
        return s;
    }
    memcpy(final, start, len);
    final[len] = '\n';
    final[len + 1] = '\0';
    free(s);
    return final;
}
