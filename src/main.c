/* rebellion CLI — scrape / crawl / map from the command line. */
#include "rebellion/rebellion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *USAGE =
"rebellion " RBL_VERSION_STRING " — a Firecrawl-style web scraper in C\n"
"\n"
"USAGE:\n"
"  rebellion scrape <url> [options]\n"
"  rebellion crawl  <url> [options]\n"
"  rebellion map    <url> [options]\n"
"\n"
"COMMON OPTIONS:\n"
"  --json                 Emit JSON instead of Markdown\n"
"  --output, -o <file>    Write output to a file instead of stdout\n"
"  --engine <mode>        http | browser | auto   (default: auto)\n"
"  --http                 Shorthand for --engine http\n"
"  --browser              Shorthand for --engine browser (headless Chromium)\n"
"  --no-robots            Do not consult robots.txt (use responsibly)\n"
"  --raw-html             Include the page HTML in output\n"
"  --no-links             Do not collect links\n"
"  --full-page-content    Keep the whole page, not just main content\n"
"  --timeout <ms>         Per-request timeout (default: 30000)\n"
"  --delay <ms>           Minimum delay between hits to one host\n"
"  --ua <string>          Override the User-Agent\n"
"  --lang <string>        Accept-Language (default: de-DE,de;q=0.9,...)\n"
"  --screenshot <file>    Save a PNG screenshot (implies --browser)\n"
"  --verbose, -v          Log progress to stderr\n"
"  --version              Print version and exit\n"
"  --help, -h             Show this help\n"
"\n"
"CRAWL OPTIONS:\n"
"  --limit <n>            Max pages to fetch (default: 50)\n"
"  --depth <n>            Max link depth from the seed (default: 2)\n"
"  --any-host             Follow links off the seed's site\n"
"  --include <regex>      Only follow URLs matching this ERE\n"
"  --exclude <regex>      Skip URLs matching this ERE\n"
"  --out-dir <dir>        Write one .md (or .json) file per crawled page\n"
"\n"
"EXAMPLES:\n"
"  rebellion scrape https://www.volksbank.de\n"
"  rebellion scrape https://example.de --json -o out.json\n"
"  rebellion crawl  https://example.de --limit 20 --depth 2 --out-dir ./out\n"
"  rebellion map    https://example.de\n";

typedef struct {
    const char *cmd;
    const char *url;
    bool json;
    const char *output;
    rbl_engine engine;
    bool engine_set;
    bool no_robots;
    bool raw_html;
    bool no_links;
    bool full_page_content;
    long timeout_ms;
    long delay_ms;
    const char *ua;
    const char *lang;
    const char *screenshot;
    bool verbose;
    /* crawl */
    int limit;
    int depth;
    bool any_host;
    const char *include;
    const char *exclude;
    const char *out_dir;
} args;

static int arg_err(const char *msg) {
    fprintf(stderr, "rebellion: %s\n", msg);
    fprintf(stderr, "Try 'rebellion --help'.\n");
    return 2;
}

static bool need_val(int argc, char **argv, int *i, const char **out) {
    if (*i + 1 >= argc) return false;
    *out = argv[++(*i)];
    return true;
}

static int write_output(const char *path, const char *data, size_t len) {
    FILE *f = path ? fopen(path, "wb") : stdout;
    if (!f) { fprintf(stderr, "rebellion: cannot open %s\n", path); return 1; }
    fwrite(data, 1, len, f);
    if (path) fclose(f);
    else fflush(stdout);
    return 0;
}

static void apply_scrape_opts(const args *a, rbl_scrape_opts *o) {
    rbl_scrape_opts_init(o);
    if (a->engine_set) o->engine = a->engine;
    o->respect_robots = !a->no_robots;
    o->include_raw_html = a->raw_html;
    o->include_links = !a->no_links;
    o->only_main_content = !a->full_page_content;
    if (a->timeout_ms > 0) o->timeout_ms = a->timeout_ms;
    if (a->ua) o->user_agent = a->ua;
    if (a->lang) o->accept_language = a->lang;
    if (a->screenshot) { o->screenshot = true; o->engine = RBL_ENGINE_BROWSER; }
}

/* ---- scrape --------------------------------------------------------- */

static int cmd_scrape(rbl_client *c, const args *a) {
    rbl_scrape_opts o;
    apply_scrape_opts(a, &o);

    rbl_status st = RBL_OK;
    rbl_doc *d = rbl_scrape_ex(c, a->url, &o, &st);
    if (!d) {
        fprintf(stderr, "rebellion: scrape failed: %s\n", rbl_strerror(st));
        return 1;
    }

    if (a->screenshot) {
        size_t n = 0;
        const unsigned char *png = rbl_doc_screenshot(d, &n);
        if (png && n) {
            FILE *f = fopen(a->screenshot, "wb");
            if (f) { fwrite(png, 1, n, f); fclose(f);
                     fprintf(stderr, "rebellion: wrote screenshot %s (%zu bytes)\n",
                             a->screenshot, n); }
        } else {
            fprintf(stderr, "rebellion: no screenshot captured\n");
        }
    }

    int rc;
    if (a->json) {
        char *js = rbl_doc_to_json(d, true);
        rc = write_output(a->output, js, strlen(js));
        free(js);
    } else {
        const char *md = rbl_doc_markdown(d);
        rc = write_output(a->output, md, md ? strlen(md) : 0);
    }
    if (a->output)
        fprintf(stderr, "rebellion: %s [%d, %s] -> %s\n", rbl_doc_url(d),
                rbl_doc_status_code(d), rbl_doc_engine_used(d), a->output);
    rbl_doc_free(d);
    return rc;
}

/* ---- crawl ---------------------------------------------------------- */

typedef struct { const args *a; int n; } crawl_ctx;

static void slugify(const char *url, char *out, size_t n) {
    size_t j = 0;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    for (; *p && j + 1 < n; p++) {
        char ch = *p;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9')) out[j++] = ch;
        else out[j++] = '_';
    }
    if (j == 0) out[j++] = 'x';
    out[j] = '\0';
}

static bool crawl_cb(const rbl_doc *doc, void *user) {
    crawl_ctx *cx = (crawl_ctx *)user;
    const args *a = cx->a;
    cx->n++;

    const char *url = rbl_doc_url(doc);
    fprintf(stderr, "  [%d] %s (%s, %d)\n", cx->n, url,
            rbl_doc_engine_used(doc), rbl_doc_status_code(doc));

    if (a->out_dir) {
        char slug[180];
        slugify(url, slug, sizeof(slug));
        char path[512];
        snprintf(path, sizeof(path), "%s/%03d_%.160s.%s", a->out_dir, cx->n,
                 slug, a->json ? "json" : "md");
        FILE *f = fopen(path, "wb");
        if (f) {
            if (a->json) { char *js = rbl_doc_to_json(doc, true);
                           fputs(js, f); free(js); }
            else { const char *md = rbl_doc_markdown(doc); if (md) fputs(md, f); }
            fclose(f);
        } else {
            fprintf(stderr, "rebellion: cannot write %s\n", path);
        }
    } else {
        if (a->json) {
            char *js = rbl_doc_to_json(doc, false);
            printf("%s\n", js);
            free(js);
        } else {
            printf("\n\n<!-- ===== %s ===== -->\n\n", url);
            const char *md = rbl_doc_markdown(doc);
            if (md) fputs(md, stdout);
        }
        fflush(stdout);
    }
    return true;
}

static int cmd_crawl(rbl_client *c, const args *a) {
    rbl_crawl_opts o;
    rbl_crawl_opts_init(&o);
    apply_scrape_opts(a, &o.scrape);
    if (a->limit > 0) o.max_pages = a->limit;
    if (a->depth >= 0) o.max_depth = a->depth;
    o.same_host_only = !a->any_host;
    o.include_regex = a->include;
    o.exclude_regex = a->exclude;

    if (a->out_dir) {
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", a->out_dir);
        int rc = system(cmd); (void)rc;
    }

    crawl_ctx cx = { a, 0 };
    fprintf(stderr, "rebellion: crawling %s (limit=%d depth=%d)\n",
            a->url, o.max_pages, o.max_depth);
    rbl_status st = rbl_crawl(c, a->url, &o, crawl_cb, &cx);
    fprintf(stderr, "rebellion: crawled %d page(s)\n", cx.n);
    if (st != RBL_OK && cx.n == 0) {
        fprintf(stderr, "rebellion: crawl failed: %s\n", rbl_strerror(st));
        return 1;
    }
    return 0;
}

/* ---- map ------------------------------------------------------------ */

static int cmd_map(rbl_client *c, const args *a) {
    rbl_strlist list;
    rbl_status st = rbl_map(c, a->url, a->limit > 0 ? a->limit : 5000, &list);
    if (st != RBL_OK) {
        fprintf(stderr, "rebellion: map failed: %s\n", rbl_strerror(st));
        return 1;
    }
    FILE *f = a->output ? fopen(a->output, "wb") : stdout;
    if (!f) { rbl_strlist_free(&list); return 1; }
    if (a->json) {
        fputs("[\n", f);
        for (size_t i = 0; i < list.count; i++)
            fprintf(f, "  \"%s\"%s\n", list.items[i], i + 1 < list.count ? "," : "");
        fputs("]\n", f);
    } else {
        for (size_t i = 0; i < list.count; i++)
            fprintf(f, "%s\n", list.items[i]);
    }
    if (a->output) fclose(f);
    fprintf(stderr, "rebellion: discovered %zu URL(s)\n", list.count);
    rbl_strlist_free(&list);
    return 0;
}

/* ---- arg parsing ---------------------------------------------------- */

int main(int argc, char **argv) {
    args a;
    memset(&a, 0, sizeof(a));
    a.engine = RBL_ENGINE_AUTO;
    a.depth = -1;

    if (argc < 2) { fputs(USAGE, stdout); return 0; }

    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (!strcmp(s, "--help") || !strcmp(s, "-h")) { fputs(USAGE, stdout); return 0; }
        if (!strcmp(s, "--version")) { printf("rebellion %s\n", RBL_VERSION_STRING); return 0; }
        if (!strcmp(s, "--json")) { a.json = true; }
        else if (!strcmp(s, "--output") || !strcmp(s, "-o")) {
            if (!need_val(argc, argv, &i, &a.output)) return arg_err("--output needs a value");
        } else if (!strcmp(s, "--engine")) {
            const char *v; if (!need_val(argc, argv, &i, &v)) return arg_err("--engine needs a value");
            if (!strcasecmp(v, "http")) a.engine = RBL_ENGINE_HTTP;
            else if (!strcasecmp(v, "browser")) a.engine = RBL_ENGINE_BROWSER;
            else if (!strcasecmp(v, "auto")) a.engine = RBL_ENGINE_AUTO;
            else return arg_err("--engine must be http|browser|auto");
            a.engine_set = true;
        } else if (!strcmp(s, "--http")) { a.engine = RBL_ENGINE_HTTP; a.engine_set = true; }
        else if (!strcmp(s, "--browser")) { a.engine = RBL_ENGINE_BROWSER; a.engine_set = true; }
        else if (!strcmp(s, "--no-robots")) a.no_robots = true;
        else if (!strcmp(s, "--raw-html")) a.raw_html = true;
        else if (!strcmp(s, "--no-links")) a.no_links = true;
        else if (!strcmp(s, "--full-page-content")) a.full_page_content = true;
        else if (!strcmp(s, "--timeout")) {
            const char *v; if (!need_val(argc, argv, &i, &v)) return arg_err("--timeout needs a value");
            a.timeout_ms = atol(v);
        } else if (!strcmp(s, "--delay")) {
            const char *v; if (!need_val(argc, argv, &i, &v)) return arg_err("--delay needs a value");
            a.delay_ms = atol(v);
        } else if (!strcmp(s, "--ua")) {
            if (!need_val(argc, argv, &i, &a.ua)) return arg_err("--ua needs a value");
        } else if (!strcmp(s, "--lang")) {
            if (!need_val(argc, argv, &i, &a.lang)) return arg_err("--lang needs a value");
        } else if (!strcmp(s, "--screenshot")) {
            if (!need_val(argc, argv, &i, &a.screenshot)) return arg_err("--screenshot needs a value");
        } else if (!strcmp(s, "--verbose") || !strcmp(s, "-v")) a.verbose = true;
        else if (!strcmp(s, "--limit")) {
            const char *v; if (!need_val(argc, argv, &i, &v)) return arg_err("--limit needs a value");
            a.limit = atoi(v);
        } else if (!strcmp(s, "--depth")) {
            const char *v; if (!need_val(argc, argv, &i, &v)) return arg_err("--depth needs a value");
            a.depth = atoi(v);
        } else if (!strcmp(s, "--any-host")) a.any_host = true;
        else if (!strcmp(s, "--include")) {
            if (!need_val(argc, argv, &i, &a.include)) return arg_err("--include needs a value");
        } else if (!strcmp(s, "--exclude")) {
            if (!need_val(argc, argv, &i, &a.exclude)) return arg_err("--exclude needs a value");
        } else if (!strcmp(s, "--out-dir")) {
            if (!need_val(argc, argv, &i, &a.out_dir)) return arg_err("--out-dir needs a value");
        } else if (s[0] == '-') {
            fprintf(stderr, "rebellion: unknown option '%s'\n", s);
            return arg_err("unknown option");
        } else if (!a.cmd) {
            a.cmd = s;
        } else if (!a.url) {
            a.url = s;
        } else {
            return arg_err("too many positional arguments");
        }
    }

    if (!a.cmd) return arg_err("no command given");
    if (!a.url) return arg_err("no URL given");
    if (strcmp(a.cmd, "scrape") && strcmp(a.cmd, "crawl") && strcmp(a.cmd, "map"))
        return arg_err("command must be scrape, crawl, or map");

    rbl_client_config cfg;
    rbl_client_config_init(&cfg);
    if (a.ua) cfg.user_agent = a.ua;
    if (a.lang) cfg.accept_language = a.lang;
    if (a.timeout_ms > 0) cfg.default_timeout_ms = a.timeout_ms;
    if (a.delay_ms > 0) cfg.min_delay_ms = a.delay_ms;
    cfg.verbose = a.verbose;

    rbl_client *c = rbl_client_new(&cfg);
    if (!c) { fprintf(stderr, "rebellion: out of memory\n"); return 1; }

    int rc;
    if (!strcmp(a.cmd, "scrape")) rc = cmd_scrape(c, &a);
    else if (!strcmp(a.cmd, "crawl")) rc = cmd_crawl(c, &a);
    else rc = cmd_map(c, &a);

    rbl_client_free(c);
    return rc;
}
