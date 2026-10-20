/*
 * rebellion — a Firecrawl-style web scraper written in C.
 *
 * Public API. Link against librebellion and include this header.
 *
 *   rbl_client *c = rbl_client_new(NULL);
 *   rbl_doc *d = rbl_scrape(c, "https://example.com", NULL);
 *   puts(rbl_doc_markdown(d));
 *   rbl_doc_free(d);
 *   rbl_client_free(c);
 */
#ifndef REBELLION_H
#define REBELLION_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RBL_VERSION_MAJOR 0
#define RBL_VERSION_MINOR 1
#define RBL_VERSION_PATCH 0
#define RBL_VERSION_STRING "0.1.0"

/* ---- error codes ---------------------------------------------------- */
typedef enum {
    RBL_OK = 0,
    RBL_ERR_INVALID_ARG,
    RBL_ERR_OOM,
    RBL_ERR_NETWORK,       /* transport failure (DNS, connect, TLS, reset) */
    RBL_ERR_HTTP,          /* a non-2xx status we chose to treat as fatal   */
    RBL_ERR_TIMEOUT,
    RBL_ERR_ROBOTS_DENIED, /* robots.txt disallows the URL (polite mode)    */
    RBL_ERR_TOO_MANY_REDIRECTS,
    RBL_ERR_BAD_URL,
    RBL_ERR_RENDER,        /* headless browser render failed                */
    RBL_ERR_PARSE,
    RBL_ERR_IO,
    RBL_ERR_BLOCKED,       /* URL blocked by the SSRF guard (private host)  */
    RBL_ERR_UNSUPPORTED
} rbl_status;

const char *rbl_strerror(rbl_status s);

/* ---- rendering strategy --------------------------------------------- */
typedef enum {
    /* Plain HTTP fetch via libcurl. Fast; no JavaScript. */
    RBL_ENGINE_HTTP = 0,
    /* Render with headless Chromium (executes JS, waits for the page to
     * settle, returns the live DOM). Needed for JS-driven pages. */
    RBL_ENGINE_BROWSER,
    /* Try HTTP first; fall back to the browser when the HTTP response looks
     * like an interstitial / JS-gate / near-empty shell. (default) */
    RBL_ENGINE_AUTO
} rbl_engine;

/* ---- per-request options -------------------------------------------- */
typedef struct rbl_scrape_opts {
    rbl_engine engine;

    /* Content selection. When only_main_content is true (default) we strip
     * nav/header/footer/aside/script/style and keep the main article body. */
    bool only_main_content;
    bool include_raw_html;   /* keep the (cleaned) HTML on the doc          */
    bool include_links;      /* collect absolute links                      */

    /* Politeness. */
    bool respect_robots;     /* obey robots.txt (default true)              */
    long max_age_ms;         /* reserved for future caching; 0 = ignore     */

    /* Timing / limits (0 = use client defaults). */
    long timeout_ms;         /* total per-URL budget                        */
    long wait_after_load_ms; /* browser: extra settle time after load       */

    /* Transport. */
    const char *user_agent;  /* override the client UA for this request     */
    const char *accept_language;

    /* Browser extras. */
    bool screenshot;         /* capture a PNG screenshot (browser engine)   */
    bool full_page;          /* full-page screenshot vs. viewport           */
} rbl_scrape_opts;

/* Fill opts with library defaults. */
void rbl_scrape_opts_init(rbl_scrape_opts *opts);

/* ---- client --------------------------------------------------------- */
typedef struct rbl_client_config {
    const char *user_agent;       /* default UA for all requests            */
    const char *accept_language;  /* default "de-DE,de;q=0.9,en;q=0.8"      */
    long default_timeout_ms;      /* default 30000                          */
    long min_delay_ms;            /* min delay between hits to one host      */
    int  max_redirects;           /* default 10                             */
    int  max_retries;             /* transient-failure retries, default 2   */
    const char *chromium_path;    /* NULL => autodetect                     */
    const char *cookie_file;      /* NULL => in-memory cookie jar           */
    bool verbose;                 /* log to stderr                          */

    /* SSRF guard. When false (default), requests to loopback, private,
     * link-local and CGNAT addresses are refused with RBL_ERR_BLOCKED.
     * Enable to scrape internal/localhost hosts you control. */
    bool allow_private_networks;
} rbl_client_config;

void rbl_client_config_init(rbl_client_config *cfg);

typedef struct rbl_client rbl_client;

/* Create a client. Pass NULL for all-defaults. Returns NULL on OOM. */
rbl_client *rbl_client_new(const rbl_client_config *cfg);
void        rbl_client_free(rbl_client *c);

/* ---- documents ------------------------------------------------------ */
typedef struct rbl_doc rbl_doc;

/* Scrape a single URL. opts may be NULL (library defaults). On failure
 * returns NULL and, if out_status != NULL, sets it. */
rbl_doc *rbl_scrape(rbl_client *c, const char *url,
                    const rbl_scrape_opts *opts);

rbl_doc *rbl_scrape_ex(rbl_client *c, const char *url,
                       const rbl_scrape_opts *opts, rbl_status *out_status);

void rbl_doc_free(rbl_doc *d);

/* Accessors (owned by the doc; valid until rbl_doc_free). */
const char *rbl_doc_url(const rbl_doc *d);          /* final URL after redirects */
const char *rbl_doc_markdown(const rbl_doc *d);
const char *rbl_doc_html(const rbl_doc *d);         /* cleaned HTML or NULL      */
const char *rbl_doc_title(const rbl_doc *d);
const char *rbl_doc_description(const rbl_doc *d);
const char *rbl_doc_language(const rbl_doc *d);
int         rbl_doc_status_code(const rbl_doc *d);  /* HTTP status              */
const char *rbl_doc_engine_used(const rbl_doc *d);  /* "http" | "browser"       */

/* Links: n via out_count. Array owned by doc. */
const char *const *rbl_doc_links(const rbl_doc *d, size_t *out_count);

/* Screenshot bytes (PNG), or NULL. */
const unsigned char *rbl_doc_screenshot(const rbl_doc *d, size_t *out_len);

/* Serialize the doc as a JSON object (heap string; caller frees). */
char *rbl_doc_to_json(const rbl_doc *d, bool pretty);

/* ---- crawl ---------------------------------------------------------- */
typedef struct rbl_crawl_opts {
    int   max_pages;         /* hard cap on pages fetched (default 50)      */
    int   max_depth;         /* link depth from the seed (default 2)        */
    bool  same_host_only;    /* stay on the seed's host (default true)      */
    bool  allow_subdomains;  /* treat sub.host as same site (default true)  */
    const char *include_regex; /* optional POSIX ERE path filter           */
    const char *exclude_regex;
    rbl_scrape_opts scrape;  /* per-page scrape options                     */
} rbl_crawl_opts;

void rbl_crawl_opts_init(rbl_crawl_opts *opts);

/* Called for each successfully scraped page during a crawl. Return false to
 * stop the crawl early. `doc` is valid only for the duration of the call. */
typedef bool (*rbl_crawl_cb)(const rbl_doc *doc, void *user);

rbl_status rbl_crawl(rbl_client *c, const char *seed_url,
                     const rbl_crawl_opts *opts,
                     rbl_crawl_cb cb, void *user);

/* ---- map (fast link discovery, no content) -------------------------- */
/* Discover URLs reachable from seed (sitemap + homepage links). Fills a
 * caller-visible, NUL-terminated list; free with rbl_strlist_free. */
typedef struct rbl_strlist {
    char **items;
    size_t count;
} rbl_strlist;

rbl_status rbl_map(rbl_client *c, const char *seed_url, int max_urls,
                   rbl_strlist *out);
void rbl_strlist_free(rbl_strlist *l);

#ifdef __cplusplus
}
#endif
#endif /* REBELLION_H */
