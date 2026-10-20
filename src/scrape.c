#include "rebellion/rebellion.h"
#include "internal.h"
#include "http.h"
#include "browser.h"
#include "html.h"
#include "markdown.h"
#include "extract.h"
#include "robots.h"
#include "url.h"
#include "net.h"
#include "json.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define RBL_UA_TOKEN "rebellion"

struct rbl_doc {
    char *url;
    int status_code;
    const char *engine; /* static string */
    char *markdown;
    char *html; /* cleaned/raw HTML or NULL */
    char *title;
    char *description;
    char *language;
    rbl_strlist links;
    unsigned char *png;
    size_t png_len;
};

/* ---- options -------------------------------------------------------- */

void rbl_scrape_opts_init(rbl_scrape_opts *o) {
    memset(o, 0, sizeof(*o));
    o->engine = RBL_ENGINE_AUTO;
    o->only_main_content = true;
    o->include_raw_html = false;
    o->include_links = true;
    o->respect_robots = true;
    o->max_age_ms = 0;
    o->timeout_ms = 0;
    o->wait_after_load_ms = 800;
    o->user_agent = NULL;
    o->accept_language = NULL;
    o->screenshot = false;
    o->full_page = false;
}

/* ---- doc construction ---------------------------------------------- */

static rbl_doc *doc_from_html(const char *html, size_t html_len, const char *base_url,
                              int status_code, const char *engine,
                              const rbl_scrape_opts *o) {
    rbl_node *root = rbl_html_parse(html, html_len);
    if (!root) return NULL;

    rbl_doc *d = (rbl_doc *)calloc(1, sizeof(*d));
    if (!d) {
        rbl_node_free(root);
        return NULL;
    }

    d->url = rbl_strdup(base_url);
    d->status_code = status_code;
    d->engine = engine;

    rbl_meta meta;
    rbl_extract_meta(root, &meta);
    d->title = meta.title;
    d->description = meta.description;
    d->language = meta.language;
    /* meta fields transferred; don't double-free */
    meta.title = meta.description = meta.language = NULL;
    rbl_meta_free(&meta);

    rbl_node *content = o->only_main_content ? rbl_find_main_content(root) : root;
    d->markdown = rbl_render_markdown(content, base_url, o->only_main_content);
    if (!d->markdown) d->markdown = rbl_strdup("");

    if (o->include_raw_html) d->html = rbl_strndup(html, html_len);

    if (o->include_links) {
        d->links.items = NULL;
        d->links.count = 0;
        rbl_collect_links(root, base_url, &d->links);
    }

    rbl_node_free(root);
    return d;
}

/* Heuristic: does an HTTP response look like it needs a real browser? */
static bool needs_browser(const rbl_http_result *r, const char *markdown) {
    long code = r->http_code;
    if (code == 403 || code == 429 || code == 503) return true;
    const char *b = r->body ? r->body : "";
    static const char *const markers[] = {"challenge-platform",
                                          "cf-browser-verification",
                                          "__cf_chl",
                                          "cf_chl_opt",
                                          "id=\"challenge-",
                                          "turnstile",
                                          "Enable JavaScript and cookies to continue",
                                          "Please enable JS",
                                          "enable JavaScript",
                                          "Bitte aktivieren Sie JavaScript",
                                          "JavaScript ist deaktiviert",
                                          "/cdn-cgi/challenge-platform",
                                          NULL};
    for (int i = 0; markers[i]; i++)
        if (strstr(b, markers[i])) return true;
    /* Very little extracted content often means a client-rendered app. */
    size_t mlen = markdown ? strlen(markdown) : 0;
    if (mlen < 160) return true;
    return false;
}

/* ---- public scrape -------------------------------------------------- */

rbl_doc *rbl_scrape_ex(rbl_client *c, const char *url, const rbl_scrape_opts *opts,
                       rbl_status *out_status) {
    rbl_status st = RBL_OK;
    rbl_doc *doc = NULL;
    if (!c || !url) {
        if (out_status) *out_status = RBL_ERR_INVALID_ARG;
        return NULL;
    }

    rbl_scrape_opts o;
    if (opts)
        o = *opts;
    else
        rbl_scrape_opts_init(&o);

    rbl_url u;
    if (!rbl_url_parse(url, &u)) {
        if (out_status) *out_status = RBL_ERR_BAD_URL;
        return NULL;
    }

    /* SSRF guard for the browser engine too (the HTTP engine checks inside
     * rbl_http_get). */
    if (!c->cfg.allow_private_networks && rbl_host_is_private(u.host)) {
        rbl_logf("SSRF guard: refusing private host %s", u.host);
        rbl_url_free(&u);
        if (out_status) *out_status = RBL_ERR_BLOCKED;
        return NULL;
    }

    if (o.respect_robots && !rbl_robots_allowed(c, &u, RBL_UA_TOKEN)) {
        rbl_logf("robots.txt disallows %s", url);
        rbl_url_free(&u);
        if (out_status) *out_status = RBL_ERR_ROBOTS_DENIED;
        return NULL;
    }

    /* Respect robots crawl-delay if larger than our default politeness. */
    long cd = o.respect_robots ? rbl_robots_crawl_delay(c, &u, RBL_UA_TOKEN) : -1;
    if (cd > c->cfg.min_delay_ms) {
        /* Temporarily honor the longer delay for this host. */
        rbl_host_state *h = rbl_client_host(c, u.host);
        if (h) {
            long long now = rbl_now_ms();
            if (h->last_hit_ms && now - h->last_hit_ms < cd)
                rbl_sleep_ms((long)(cd - (now - h->last_hit_ms)));
        }
    }
    rbl_client_rate_limit(c, u.host);

    rbl_engine engine = o.engine;

    if (engine == RBL_ENGINE_HTTP || engine == RBL_ENGINE_AUTO) {
        rbl_http_result r;
        rbl_status hs =
            rbl_http_get(c, u.full, o.user_agent, o.accept_language, o.timeout_ms, &r);
        if (hs == RBL_OK && r.is_html && r.body) {
            doc = doc_from_html(r.body, r.body_len, r.final_url, (int)r.http_code,
                                "http", &o);
            if (engine == RBL_ENGINE_AUTO && doc && needs_browser(&r, doc->markdown) &&
                rbl_browser_available(c)) {
                rbl_logf("auto: HTTP looked gated/empty, escalating to browser");
                rbl_http_result_free(&r);
                engine = RBL_ENGINE_BROWSER; /* fall through to render */
                rbl_doc_free(doc);
                doc = NULL;
            } else {
                rbl_http_result_free(&r);
                if (doc) {
                    rbl_url_free(&u);
                    if (out_status) *out_status = RBL_OK;
                    return doc;
                }
                st = RBL_ERR_PARSE;
            }
        } else if (hs == RBL_OK && !r.is_html) {
            /* Non-HTML (e.g. JSON, text). Return it verbatim as markdown. */
            doc = (rbl_doc *)calloc(1, sizeof(*doc));
            if (doc) {
                doc->url = rbl_strdup(r.final_url);
                doc->status_code = (int)r.http_code;
                doc->engine = "http";
                doc->markdown = rbl_strdup(r.body ? r.body : "");
                if (o.include_raw_html) doc->html = rbl_strdup(r.body ? r.body : "");
            }
            rbl_http_result_free(&r);
            rbl_url_free(&u);
            if (out_status) *out_status = doc ? RBL_OK : RBL_ERR_OOM;
            return doc;
        } else {
            st = hs;
            rbl_http_result_free(&r);
            /* Don't escalate a blocked host to the browser. */
            if (engine == RBL_ENGINE_AUTO && hs != RBL_ERR_BLOCKED &&
                rbl_browser_available(c)) {
                rbl_logf("auto: HTTP failed (%s), trying browser", rbl_strerror(hs));
                engine = RBL_ENGINE_BROWSER;
            }
        }
    }

    if (engine == RBL_ENGINE_BROWSER) {
        if (!rbl_browser_available(c)) {
            st = RBL_ERR_UNSUPPORTED;
        } else {
            rbl_render_opts ro;
            memset(&ro, 0, sizeof(ro));
            ro.timeout_ms = o.timeout_ms > 0 ? o.timeout_ms : 45000;
            ro.settle_ms = o.wait_after_load_ms;
            ro.user_agent = o.user_agent;
            ro.accept_language = o.accept_language;
            ro.screenshot = o.screenshot;
            ro.full_page = o.full_page;

            char *html = NULL, *final_url = NULL;
            unsigned char *png = NULL;
            size_t png_len = 0;
            rbl_status rs =
                rbl_browser_render(c, u.full, &ro, &html, &final_url, &png, &png_len);
            if (rs == RBL_OK && html) {
                doc = doc_from_html(html, strlen(html), final_url ? final_url : u.full,
                                    200, "browser", &o);
                if (doc && png) {
                    doc->png = png;
                    doc->png_len = png_len;
                    png = NULL;
                }
                st = doc ? RBL_OK : RBL_ERR_OOM;
            } else {
                st = (rs == RBL_OK) ? RBL_ERR_RENDER : rs;
            }
            free(html);
            free(final_url);
            free(png);
        }
    }

    rbl_url_free(&u);
    if (out_status) *out_status = doc ? RBL_OK : st;
    return doc;
}

rbl_doc *rbl_scrape(rbl_client *c, const char *url, const rbl_scrape_opts *opts) {
    return rbl_scrape_ex(c, url, opts, NULL);
}

rbl_doc *rbl_session_scrape(rbl_session *s, const rbl_scrape_opts *opts) {
    if (!s) return NULL;
    char *html = rbl_session_html(s);
    if (!html) return NULL;
    const char *url = rbl_session_url(s);
    rbl_scrape_opts o;
    if (opts)
        o = *opts;
    else
        rbl_scrape_opts_init(&o);
    rbl_doc *d = doc_from_html(html, strlen(html), url ? url : "", 200, "browser", &o);
    free(html);
    return d;
}

/* ---- doc accessors -------------------------------------------------- */

void rbl_doc_free(rbl_doc *d) {
    if (!d) return;
    free(d->url);
    free(d->markdown);
    free(d->html);
    free(d->title);
    free(d->description);
    free(d->language);
    rbl_strlist_free(&d->links);
    free(d->png);
    free(d);
}

const char *rbl_doc_url(const rbl_doc *d) {
    return d ? d->url : NULL;
}
const char *rbl_doc_markdown(const rbl_doc *d) {
    return d ? d->markdown : NULL;
}
const char *rbl_doc_html(const rbl_doc *d) {
    return d ? d->html : NULL;
}
const char *rbl_doc_title(const rbl_doc *d) {
    return d ? d->title : NULL;
}
const char *rbl_doc_description(const rbl_doc *d) {
    return d ? d->description : NULL;
}
const char *rbl_doc_language(const rbl_doc *d) {
    return d ? d->language : NULL;
}
int rbl_doc_status_code(const rbl_doc *d) {
    return d ? d->status_code : 0;
}
const char *rbl_doc_engine_used(const rbl_doc *d) {
    return d ? d->engine : NULL;
}

const char *const *rbl_doc_links(const rbl_doc *d, size_t *out_count) {
    if (!d) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    if (out_count) *out_count = d->links.count;
    return (const char *const *)d->links.items;
}

const unsigned char *rbl_doc_screenshot(const rbl_doc *d, size_t *out_len) {
    if (!d || !d->png) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    if (out_len) *out_len = d->png_len;
    return d->png;
}

/* ---- JSON ----------------------------------------------------------- */

static void json_kv(rbl_buf *b, const char *key, const char *val, bool *first,
                    bool pretty) {
    if (!val) return;
    if (!*first) rbl_buf_append_str(b, pretty ? ",\n" : ",");
    *first = false;
    if (pretty) rbl_buf_append_str(b, "  ");
    rbl_json_quote(b, key);
    rbl_buf_append_str(b, ": ");
    rbl_json_quote(b, val);
}

char *rbl_doc_to_json(const rbl_doc *d, bool pretty) {
    if (!d) return NULL;
    rbl_buf b;
    rbl_buf_init(&b);
    rbl_buf_append_str(&b, pretty ? "{\n" : "{");
    bool first = true;

    json_kv(&b, "url", d->url, &first, pretty);

    if (!first) rbl_buf_append_str(&b, pretty ? ",\n" : ",");
    first = false;
    if (pretty) rbl_buf_append_str(&b, "  ");
    rbl_json_quote(&b, "statusCode");
    rbl_buf_appendf(&b, ": %d", d->status_code);

    if (!first) rbl_buf_append_str(&b, pretty ? ",\n" : ",");
    if (pretty) rbl_buf_append_str(&b, "  ");
    rbl_json_quote(&b, "engine");
    rbl_buf_append_str(&b, ": ");
    rbl_json_quote(&b, d->engine ? d->engine : "http");

    json_kv(&b, "title", d->title, &first, pretty);
    json_kv(&b, "description", d->description, &first, pretty);
    json_kv(&b, "language", d->language, &first, pretty);
    json_kv(&b, "markdown", d->markdown, &first, pretty);
    if (d->html) json_kv(&b, "html", d->html, &first, pretty);

    /* links array */
    rbl_buf_append_str(&b, pretty ? ",\n  " : ",");
    rbl_json_quote(&b, "links");
    rbl_buf_append_str(&b, ": [");
    for (size_t i = 0; i < d->links.count; i++) {
        if (i) rbl_buf_append_str(&b, ",");
        if (pretty) rbl_buf_append_str(&b, "\n    ");
        rbl_json_quote(&b, d->links.items[i]);
    }
    if (pretty && d->links.count) rbl_buf_append_str(&b, "\n  ");
    rbl_buf_append_str(&b, "]");

    if (d->png && d->png_len) {
        char *b64 = rbl_base64_encode(d->png, d->png_len);
        if (b64) {
            rbl_buf_append_str(&b, pretty ? ",\n  " : ",");
            rbl_json_quote(&b, "screenshot");
            rbl_buf_append_str(&b, ": ");
            rbl_buf_append_ch(&b, '"');
            rbl_buf_append_str(&b, "data:image/png;base64,");
            rbl_buf_append_str(&b, b64);
            rbl_buf_append_ch(&b, '"');
            free(b64);
        }
    }

    rbl_buf_append_str(&b, pretty ? "\n}\n" : "}");
    return rbl_buf_steal(&b);
}

/* ---- strlist -------------------------------------------------------- */

void rbl_strlist_free(rbl_strlist *l) {
    if (!l) return;
    for (size_t i = 0; i < l->count; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = 0;
}

/* ---- error strings -------------------------------------------------- */

const char *rbl_strerror(rbl_status s) {
    switch (s) {
    case RBL_OK:
        return "ok";
    case RBL_ERR_INVALID_ARG:
        return "invalid argument";
    case RBL_ERR_OOM:
        return "out of memory";
    case RBL_ERR_NETWORK:
        return "network error";
    case RBL_ERR_HTTP:
        return "HTTP error";
    case RBL_ERR_TIMEOUT:
        return "timeout";
    case RBL_ERR_ROBOTS_DENIED:
        return "disallowed by robots.txt";
    case RBL_ERR_TOO_MANY_REDIRECTS:
        return "too many redirects";
    case RBL_ERR_BAD_URL:
        return "bad URL";
    case RBL_ERR_RENDER:
        return "browser render failed";
    case RBL_ERR_PARSE:
        return "parse error";
    case RBL_ERR_IO:
        return "I/O error";
    case RBL_ERR_BLOCKED:
        return "blocked by SSRF guard (private host)";
    case RBL_ERR_UNSUPPORTED:
        return "unsupported";
    default:
        return "unknown error";
    }
}
