#include "rebellion/rebellion.h"
#include "internal.h"
#include "http.h"
#include "html.h"
#include "extract.h"
#include "url.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <regex.h>

void rbl_crawl_opts_init(rbl_crawl_opts *o) {
    memset(o, 0, sizeof(*o));
    o->max_pages = 50;
    o->max_depth = 2;
    o->same_host_only = true;
    o->allow_subdomains = true;
    o->include_regex = NULL;
    o->exclude_regex = NULL;
    rbl_scrape_opts_init(&o->scrape);
}

/* ---- queue of (url, depth) ------------------------------------------ */

typedef struct {
    char *url;
    int depth;
} qitem;
typedef struct {
    qitem *items;
    size_t head, count, cap;
} queue;

static bool q_push(queue *q, const char *url, int depth) {
    if (q->count == q->cap) {
        size_t nc = q->cap ? q->cap * 2 : 64;
        qitem *ni = realloc(q->items, nc * sizeof(qitem));
        if (!ni) return false;
        q->items = ni;
        q->cap = nc;
    }
    q->items[q->count].url = rbl_strdup(url);
    q->items[q->count].depth = depth;
    q->count++;
    return true;
}
static bool q_pop(queue *q, qitem *out) {
    if (q->head >= q->count) return false;
    *out = q->items[q->head++];
    return true;
}
static void q_free(queue *q) {
    for (size_t i = q->head; i < q->count; i++) free(q->items[i].url);
    free(q->items);
}

/* ---- crawl ---------------------------------------------------------- */

rbl_status rbl_crawl(rbl_client *c, const char *seed_url, const rbl_crawl_opts *opts_in,
                     rbl_crawl_cb cb, void *user) {
    if (!c || !seed_url || !cb) return RBL_ERR_INVALID_ARG;
    rbl_crawl_opts o;
    if (opts_in)
        o = *opts_in;
    else
        rbl_crawl_opts_init(&o);

    rbl_url seed;
    if (!rbl_url_parse(seed_url, &seed)) return RBL_ERR_BAD_URL;

    regex_t inc, exc;
    bool have_inc = false, have_exc = false;
    if (o.include_regex &&
        regcomp(&inc, o.include_regex, REG_EXTENDED | REG_NOSUB) == 0)
        have_inc = true;
    if (o.exclude_regex &&
        regcomp(&exc, o.exclude_regex, REG_EXTENDED | REG_NOSUB) == 0)
        have_exc = true;

    queue q;
    memset(&q, 0, sizeof(q));
    rbl_strset visited;
    rbl_strset_init(&visited);

    char *seed_clean = rbl_url_strip_fragment(seed.full);
    q_push(&q, seed_clean, 0);
    rbl_strset_add(&visited, seed_clean);
    free(seed_clean);

    /* The crawl scope host. If the seed redirects to another host, we adopt
     * the host we actually landed on (set from the first fetched page). */
    char *scope_host = rbl_strdup(seed.host);

    int fetched = 0;
    rbl_status result = RBL_OK;
    qitem it;
    while (fetched < o.max_pages && q_pop(&q, &it)) {
        rbl_logf("crawl[%d/%d] depth=%d %s", fetched + 1, o.max_pages, it.depth,
                 it.url);

        rbl_status st = RBL_OK;
        rbl_doc *doc = rbl_scrape_ex(c, it.url, &o.scrape, &st);
        if (doc) {
            fetched++;
            bool keep_going = cb(doc, user);

            /* Adopt the landed host from the first page (handles a seed that
             * redirects to another domain). */
            if (fetched == 1) {
                rbl_url fu;
                if (rbl_url_parse(rbl_doc_url(doc), &fu)) {
                    if (fu.host && !rbl_host_matches(fu.host, scope_host, false)) {
                        free(scope_host);
                        scope_host = rbl_strdup(fu.host);
                        rbl_logf("crawl: scope host -> %s (after redirect)",
                                 scope_host);
                    }
                    rbl_url_free(&fu);
                }
            }

            if (it.depth < o.max_depth) {
                size_t nlinks = 0;
                const char *const *links = rbl_doc_links(doc, &nlinks);
                for (size_t i = 0; i < nlinks; i++) {
                    const char *link = links[i];
                    rbl_url lu;
                    if (!rbl_url_parse(link, &lu)) continue;
                    bool ok = true;
                    if (o.same_host_only &&
                        !rbl_host_matches(lu.host, scope_host, o.allow_subdomains))
                        ok = false;
                    if (ok && have_inc && regexec(&inc, link, 0, NULL, 0) != 0)
                        ok = false;
                    if (ok && have_exc && regexec(&exc, link, 0, NULL, 0) == 0)
                        ok = false;
                    rbl_url_free(&lu);
                    if (!ok) continue;
                    if (rbl_strset_add(&visited, link)) q_push(&q, link, it.depth + 1);
                }
            }
            rbl_doc_free(doc);
            if (!keep_going) {
                free(it.url);
                break;
            }
        } else {
            rbl_logf("crawl: skip %s (%s)", it.url, rbl_strerror(st));
        }
        free(it.url);
    }

    free(scope_host);
    q_free(&q);
    rbl_strset_free(&visited);
    if (have_inc) regfree(&inc);
    if (have_exc) regfree(&exc);
    rbl_url_free(&seed);
    return result;
}

/* ---- map: fast URL discovery ---------------------------------------- */

/* Extract <loc>...</loc> URLs from sitemap XML into the set/list. */
static void harvest_sitemap(const char *xml, rbl_strlist *out, rbl_strset *seen,
                            int max_urls) {
    const char *p = xml;
    while ((p = strstr(p, "<loc>")) != NULL && (int)out->count < max_urls) {
        p += 5;
        const char *e = strstr(p, "</loc>");
        if (!e) break;
        char *raw = rbl_strndup(p, (size_t)(e - p));
        char *u = rbl_trim(raw);
        /* decode a couple of common XML entities in URLs */
        rbl_buf b;
        rbl_buf_init(&b);
        for (char *q = u; *q; q++) {
            if (strncmp(q, "&amp;", 5) == 0) {
                rbl_buf_append_ch(&b, '&');
                q += 4;
            } else
                rbl_buf_append_ch(&b, *q);
        }
        char *clean = rbl_buf_steal(&b);
        if (clean &&
            (strncmp(clean, "http://", 7) == 0 || strncmp(clean, "https://", 8) == 0)) {
            if (rbl_strset_add(seen, clean)) {
                char **ni = realloc(out->items, (out->count + 1) * sizeof(char *));
                if (ni) {
                    out->items = ni;
                    out->items[out->count++] = clean;
                    clean = NULL;
                }
            }
        }
        free(clean);
        free(raw);
        p = e + 6;
    }
}

rbl_status rbl_map(rbl_client *c, const char *seed_url, int max_urls,
                   rbl_strlist *out) {
    if (!c || !seed_url || !out) return RBL_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (max_urls <= 0) max_urls = 5000;

    rbl_url seed;
    if (!rbl_url_parse(seed_url, &seed)) return RBL_ERR_BAD_URL;

    rbl_strset seen;
    rbl_strset_init(&seen);

    /* 1) sitemap.xml (and nested sitemaps one level deep). */
    rbl_buf sm;
    rbl_buf_init(&sm);
    rbl_buf_appendf(&sm, "%s://%s/sitemap.xml", seed.scheme, seed.host);
    rbl_http_result r;
    if (rbl_http_get(c, rbl_buf_cstr(&sm), NULL, NULL, 15000, &r) == RBL_OK &&
        r.http_code >= 200 && r.http_code < 300 && r.body) {
        if (strstr(r.body, "<sitemapindex")) {
            /* index of sitemaps: fetch each child <loc> */
            rbl_strlist children;
            memset(&children, 0, sizeof(children));
            rbl_strset cseen;
            rbl_strset_init(&cseen);
            harvest_sitemap(r.body, &children, &cseen, 200);
            for (size_t i = 0; i < children.count && (int)out->count < max_urls; i++) {
                rbl_http_result cr;
                if (rbl_http_get(c, children.items[i], NULL, NULL, 15000, &cr) ==
                        RBL_OK &&
                    cr.body)
                    harvest_sitemap(cr.body, out, &seen, max_urls);
                rbl_http_result_free(&cr);
            }
            rbl_strlist_free(&children);
            rbl_strset_free(&cseen);
        } else {
            harvest_sitemap(r.body, out, &seen, max_urls);
        }
    }
    rbl_http_result_free(&r);
    rbl_buf_free(&sm);

    /* 2) homepage links (same host). */
    if ((int)out->count < max_urls) {
        rbl_http_result hr;
        if (rbl_http_get(c, seed.full, NULL, NULL, 20000, &hr) == RBL_OK &&
            hr.is_html && hr.body) {
            rbl_node *root = rbl_html_parse(hr.body, hr.body_len);
            if (root) {
                rbl_strlist links;
                memset(&links, 0, sizeof(links));
                rbl_collect_links(root, hr.final_url, &links);
                for (size_t i = 0; i < links.count && (int)out->count < max_urls; i++) {
                    rbl_url lu;
                    if (!rbl_url_parse(links.items[i], &lu)) continue;
                    bool same = rbl_host_matches(lu.host, seed.host, true);
                    rbl_url_free(&lu);
                    if (same && rbl_strset_add(&seen, links.items[i])) {
                        char **ni =
                            realloc(out->items, (out->count + 1) * sizeof(char *));
                        if (ni) {
                            out->items = ni;
                            out->items[out->count++] = rbl_strdup(links.items[i]);
                        }
                    }
                }
                rbl_strlist_free(&links);
                rbl_node_free(root);
            }
        }
        rbl_http_result_free(&hr);
    }

    rbl_strset_free(&seen);
    rbl_url_free(&seed);
    return RBL_OK;
}
