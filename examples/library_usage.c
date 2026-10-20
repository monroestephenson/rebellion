/*
 * Example: use librebellion to scrape a page, print its metadata and
 * Markdown, then crawl a couple of pages.
 *
 * Build (after building the library in ../build):
 *   cc library_usage.c -I../include -L../build -lrebellion -lcurl -o example
 * Run:
 *   ./example https://www.vr.de
 */
#include <rebellion/rebellion.h>
#include <stdio.h>
#include <string.h>

static bool on_page(const rbl_doc *d, void *user) {
    int *n = (int *)user;
    (*n)++;
    printf("  crawled: %s  [%d, %s]\n", rbl_doc_url(d), rbl_doc_status_code(d),
           rbl_doc_engine_used(d));
    return *n < 3; /* stop after 3 pages */
}

int main(int argc, char **argv) {
    const char *url = argc > 1 ? argv[1] : "https://www.vr.de";

    rbl_client_config cfg;
    rbl_client_config_init(&cfg);
    cfg.verbose = false;

    rbl_client *c = rbl_client_new(&cfg);
    if (!c) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    /* --- single page --- */
    rbl_scrape_opts o;
    rbl_scrape_opts_init(&o);
    o.engine = RBL_ENGINE_AUTO;

    rbl_status st;
    rbl_doc *d = rbl_scrape_ex(c, url, &o, &st);
    if (!d) {
        fprintf(stderr, "scrape failed: %s\n", rbl_strerror(st));
        rbl_client_free(c);
        return 1;
    }

    printf("URL:      %s\n", rbl_doc_url(d));
    printf("Title:    %s\n", rbl_doc_title(d) ? rbl_doc_title(d) : "(none)");
    printf("Language: %s\n", rbl_doc_language(d) ? rbl_doc_language(d) : "(none)");

    size_t nlinks = 0;
    rbl_doc_links(d, &nlinks);
    printf("Links:    %zu\n", nlinks);

    const char *md = rbl_doc_markdown(d);
    size_t preview = md ? strlen(md) : 0;
    if (preview > 500) preview = 500;
    printf("\n--- Markdown (first %zu chars) ---\n%.*s\n", preview, (int)preview,
           md ? md : "");

    rbl_doc_free(d);

    /* --- crawl --- */
    printf("\n--- Crawl (max 3 pages, depth 1) ---\n");
    rbl_crawl_opts co;
    rbl_crawl_opts_init(&co);
    co.max_pages = 3;
    co.max_depth = 1;
    co.scrape.engine = RBL_ENGINE_HTTP;
    int n = 0;
    rbl_crawl(c, url, &co, on_page, &n);
    printf("  total: %d page(s)\n", n);

    rbl_client_free(c);
    return 0;
}
