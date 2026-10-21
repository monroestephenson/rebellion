/* rebellion CLI — scrape / crawl / map from the command line. */
#include "rebellion/rebellion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

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
    "  --allow-private        Allow scraping loopback/private/internal hosts\n"
    "  --raw-html             Include the page HTML in output\n"
    "  --no-links             Do not collect links\n"
    "  --full-page-content    Keep the whole page, not just main content\n"
    "  --timeout <ms>         Per-request timeout (default: 30000)\n"
    "  --delay <ms>           Minimum delay between hits to one host\n"
    "  --ua <string>          Override the User-Agent\n"
    "  --lang <string>        Accept-Language (default: de-DE,de;q=0.9,...)\n"
    "  --screenshot <file>    Save a PNG screenshot (implies --browser)\n"
    "  --full-page            Screenshot the whole page, not just the viewport\n"
    "  --proxy <url>          Proxy URL; repeat to build a rotating pool\n"
    "  --proxy-strategy <s>   round-robin (default) | random | sticky\n"
    "  --profile <dir>        Persistent browser profile (keeps logins/cookies)\n"
    "  --verbose, -v          Log progress to stderr\n"
    "  --version              Print version and exit\n"
    "  --help, -h             Show this help\n"
    "\n"
    "BROWSER ACTIONS (scrape; run in order before capture, imply --browser):\n"
    "  --wait-for <selector>  Wait until a CSS selector appears\n"
    "  --click <selector>     Click the first matching element\n"
    "  --fill <selector=val>  Type a value into a field\n"
    "  --submit <selector>    Submit a form (or its field/button)\n"
    "  --scroll-bottom        Scroll to the bottom (trigger lazy loading)\n"
    "  --wait <ms>            Pause for N milliseconds\n"
    "  A failing action is reported and skipped (e.g. a cookie banner that is\n"
    "  not shown); a navigation blocked by the private-network guard is fatal.\n"
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
    "  rebellion scrape https://shop.de --click '#accept' --scroll-bottom\n"
    "  rebellion scrape https://app.de --fill '#q=laptop' --submit '#search'\n"
    "  rebellion crawl  https://example.de --limit 20 --depth 2 --out-dir ./out\n"
    "  rebellion map    https://example.de\n";

#define RBL_MAX_ACTIONS 64
#define RBL_MAX_PROXIES 64

typedef enum {
    ACT_WAIT_FOR,
    ACT_CLICK,
    ACT_FILL,
    ACT_SUBMIT,
    ACT_SCROLL_BOTTOM,
    ACT_WAIT
} act_kind;

typedef struct {
    act_kind kind;
    const char *a; /* selector, or "selector=value" for --fill */
    long n;        /* ms (for --wait) */
} action;

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
    bool full_page;
    bool verbose;
    /* crawl */
    int limit;
    int depth;
    bool any_host;
    const char *include;
    const char *exclude;
    const char *out_dir;
    bool allow_private;
    /* proxy + profile */
    const char *proxies[RBL_MAX_PROXIES];
    int n_proxies;
    const char *proxy_strategy;
    const char *profile;
    /* ordered browser actions (scrape) */
    action actions[RBL_MAX_ACTIONS];
    int n_actions;
} args;

/* mkdir -p, pure C. Returns 0 on success. */
static int mkdir_p(const char *path) {
    char tmp[1024];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);
    if (tmp[n - 1] == '/') tmp[n - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

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
    if (!f) {
        fprintf(stderr, "rebellion: cannot open %s\n", path);
        return 1;
    }
    fwrite(data, 1, len, f);
    if (path)
        fclose(f);
    else
        fflush(stdout);
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
    if (a->screenshot) {
        o->screenshot = true;
        o->full_page = a->full_page;
        o->engine = RBL_ENGINE_BROWSER;
    }
}

/* Strict integer parsing for option values. */
static bool parse_long(const char *s, long lo, long hi, long *out) {
    char *e = NULL;
    errno = 0;
    long v = strtol(s, &e, 10);
    if (!*s || *e || errno || v < lo || v > hi) return false;
    *out = v;
    return true;
}

/* Write s as a JSON string literal. */
static void json_puts(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\')
            fprintf(f, "\\%c", *p);
        else if (*p < 0x20)
            fprintf(f, "\\u%04x", *p);
        else
            fputc(*p, f);
    }
    fputc('"', f);
}

/* ---- scrape --------------------------------------------------------- */

static void save_png(const char *path, const unsigned char *png, size_t n) {
    if (png && n) {
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(png, 1, n, f);
            fclose(f);
            fprintf(stderr, "rebellion: wrote screenshot %s (%zu bytes)\n", path, n);
        } else {
            fprintf(stderr, "rebellion: cannot write screenshot %s\n", path);
        }
    } else {
        fprintf(stderr, "rebellion: no screenshot captured\n");
    }
}

static int write_doc_output(const args *a, rbl_doc *d) {
    int rc;
    if (a->json) {
        char *js = rbl_doc_to_json(d, true);
        if (!js) {
            fprintf(stderr, "rebellion: out of memory\n");
            return 1;
        }
        rc = write_output(a->output, js, strlen(js));
        free(js);
    } else {
        const char *md = rbl_doc_markdown(d);
        rc = write_output(a->output, md, md ? strlen(md) : 0);
    }
    if (a->output)
        fprintf(stderr, "rebellion: %s [%d, %s] -> %s\n", rbl_doc_url(d),
                rbl_doc_status_code(d), rbl_doc_engine_used(d), a->output);
    return rc;
}

/* Drive an interactive session: navigate, run the ordered actions, then
 * capture. Used when the user passed browser actions. */
static int scrape_with_actions(rbl_client *c, const args *a) {
    rbl_session_opts so;
    rbl_session_opts_init(&so);
    if (a->ua) so.user_agent = a->ua;
    if (a->lang) so.accept_language = a->lang;
    if (a->timeout_ms > 0) so.timeout_ms = a->timeout_ms;
    so.respect_robots = !a->no_robots;

    rbl_status st;
    rbl_session *s = rbl_session_open(c, &so, &st);
    if (!s) {
        fprintf(stderr, "rebellion: session open failed: %s\n", rbl_strerror(st));
        return 1;
    }
    st = rbl_session_goto(s, a->url, -1);
    if (st != RBL_OK) {
        fprintf(stderr, "rebellion: navigate failed: %s\n", rbl_strerror(st));
        rbl_session_close(s);
        return 1;
    }

    for (int k = 0; k < a->n_actions; k++) {
        const action *ac = &a->actions[k];
        rbl_status as = RBL_OK;
        const char *name = "";
        char argbuf[64] = {0};
        switch (ac->kind) {
        case ACT_WAIT_FOR:
            name = "wait-for";
            as = rbl_session_wait_for(s, ac->a, 0);
            break;
        case ACT_CLICK:
            name = "click";
            as = rbl_session_click(s, ac->a);
            break;
        case ACT_SUBMIT:
            name = "submit";
            as = rbl_session_submit(s, ac->a);
            break;
        case ACT_FILL: {
            name = "fill";
            const char *eq = strchr(ac->a, '=');
            size_t sl = (size_t)(eq - ac->a);
            char sel[512];
            if (sl >= sizeof(sel)) sl = sizeof(sel) - 1;
            memcpy(sel, ac->a, sl);
            sel[sl] = '\0';
            as = rbl_session_fill(s, sel, eq + 1);
            break;
        }
        case ACT_SCROLL_BOTTOM:
            name = "scroll-bottom";
            as = rbl_session_scroll_to_bottom(s);
            break;
        case ACT_WAIT:
            name = "wait";
            snprintf(argbuf, sizeof(argbuf), "%ldms", ac->n);
            as = rbl_session_wait_ms(s, ac->n);
            break;
        }
        if (as == RBL_OK) as = rbl_session_last_error(s); /* blocked navigation */
        if (a->verbose || as != RBL_OK) {
            const char *detail = ac->a ? ac->a : (argbuf[0] ? argbuf : "");
            fprintf(stderr, "rebellion: action %s%s%s -> %s\n", name,
                    detail[0] ? " " : "", detail, rbl_strerror(as));
        }
        if (as == RBL_ERR_BLOCKED) {
            rbl_session_close(s);
            return 1;
        }
    }

    if (a->screenshot) {
        size_t n = 0;
        unsigned char *png = rbl_session_screenshot(s, a->full_page, &n);
        save_png(a->screenshot, png, n);
        free(png);
    }

    rbl_scrape_opts o;
    apply_scrape_opts(a, &o);
    rbl_doc *d = rbl_session_scrape(s, &o);
    rbl_session_close(s);
    if (!d) {
        fprintf(stderr, "rebellion: capture failed\n");
        return 1;
    }
    int rc = write_doc_output(a, d);
    rbl_doc_free(d);
    return rc;
}

static int cmd_scrape(rbl_client *c, const args *a) {
    if (a->n_actions > 0) return scrape_with_actions(c, a);

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
        save_png(a->screenshot, png, n);
    }

    int rc = write_doc_output(a, d);
    rbl_doc_free(d);
    return rc;
}

/* ---- crawl ---------------------------------------------------------- */

typedef struct {
    const args *a;
    int n;
} crawl_ctx;

static void slugify(const char *url, char *out, size_t n) {
    size_t j = 0;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    for (; *p && j + 1 < n; p++) {
        char ch = *p;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9'))
            out[j++] = ch;
        else
            out[j++] = '_';
    }
    if (j == 0) out[j++] = 'x';
    out[j] = '\0';
}

static bool crawl_cb(const rbl_doc *doc, void *user) {
    crawl_ctx *cx = (crawl_ctx *)user;
    const args *a = cx->a;
    cx->n++;

    const char *url = rbl_doc_url(doc);
    fprintf(stderr, "  [%d] %s (%s, %d)\n", cx->n, url, rbl_doc_engine_used(doc),
            rbl_doc_status_code(doc));

    if (a->out_dir) {
        char slug[180];
        slugify(url, slug, sizeof(slug));
        char path[512];
        snprintf(path, sizeof(path), "%s/%03d_%.160s.%s", a->out_dir, cx->n, slug,
                 a->json ? "json" : "md");
        FILE *f = fopen(path, "wb");
        if (f) {
            if (a->json) {
                char *js = rbl_doc_to_json(doc, true);
                if (js) fputs(js, f);
                free(js);
            } else {
                const char *md = rbl_doc_markdown(doc);
                if (md) fputs(md, f);
            }
            fclose(f);
        } else {
            fprintf(stderr, "rebellion: cannot write %s\n", path);
        }
    } else {
        if (a->json) {
            char *js = rbl_doc_to_json(doc, false);
            if (js) printf("%s\n", js);
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

    if (a->out_dir && mkdir_p(a->out_dir) != 0) {
        fprintf(stderr, "rebellion: cannot create --out-dir %s\n", a->out_dir);
        return 1;
    }

    crawl_ctx cx = {a, 0};
    fprintf(stderr, "rebellion: crawling %s (limit=%d depth=%d)\n", a->url, o.max_pages,
            o.max_depth);
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
    if (!f) {
        fprintf(stderr, "rebellion: cannot open %s\n", a->output);
        rbl_strlist_free(&list);
        return 1;
    }
    if (a->json) {
        fputs("[\n", f);
        for (size_t i = 0; i < list.count; i++) {
            fputs("  ", f);
            json_puts(f, list.items[i]);
            fputs(i + 1 < list.count ? ",\n" : "\n", f);
        }
        fputs("]\n", f);
    } else {
        for (size_t i = 0; i < list.count; i++) fprintf(f, "%s\n", list.items[i]);
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

    if (argc < 2) {
        fputs(USAGE, stdout);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (!strcmp(s, "--help") || !strcmp(s, "-h")) {
            fputs(USAGE, stdout);
            return 0;
        }
        if (!strcmp(s, "--version")) {
            printf("rebellion %s\n", RBL_VERSION_STRING);
            return 0;
        }
        if (!strcmp(s, "--json")) {
            a.json = true;
        } else if (!strcmp(s, "--output") || !strcmp(s, "-o")) {
            if (!need_val(argc, argv, &i, &a.output))
                return arg_err("--output needs a value");
        } else if (!strcmp(s, "--engine")) {
            const char *v;
            if (!need_val(argc, argv, &i, &v)) return arg_err("--engine needs a value");
            if (!strcasecmp(v, "http"))
                a.engine = RBL_ENGINE_HTTP;
            else if (!strcasecmp(v, "browser"))
                a.engine = RBL_ENGINE_BROWSER;
            else if (!strcasecmp(v, "auto"))
                a.engine = RBL_ENGINE_AUTO;
            else
                return arg_err("--engine must be http|browser|auto");
            a.engine_set = true;
        } else if (!strcmp(s, "--http")) {
            a.engine = RBL_ENGINE_HTTP;
            a.engine_set = true;
        } else if (!strcmp(s, "--browser")) {
            a.engine = RBL_ENGINE_BROWSER;
            a.engine_set = true;
        } else if (!strcmp(s, "--no-robots"))
            a.no_robots = true;
        else if (!strcmp(s, "--allow-private"))
            a.allow_private = true;
        else if (!strcmp(s, "--raw-html"))
            a.raw_html = true;
        else if (!strcmp(s, "--no-links"))
            a.no_links = true;
        else if (!strcmp(s, "--full-page-content"))
            a.full_page_content = true;
        else if (!strcmp(s, "--timeout")) {
            const char *v;
            if (!need_val(argc, argv, &i, &v) ||
                !parse_long(v, 1, 3600000, &a.timeout_ms))
                return arg_err("--timeout needs a number of milliseconds");
        } else if (!strcmp(s, "--delay")) {
            const char *v;
            if (!need_val(argc, argv, &i, &v) ||
                !parse_long(v, 0, 3600000, &a.delay_ms))
                return arg_err("--delay needs a number of milliseconds");
        } else if (!strcmp(s, "--ua")) {
            if (!need_val(argc, argv, &i, &a.ua)) return arg_err("--ua needs a value");
        } else if (!strcmp(s, "--lang")) {
            if (!need_val(argc, argv, &i, &a.lang))
                return arg_err("--lang needs a value");
        } else if (!strcmp(s, "--screenshot")) {
            if (!need_val(argc, argv, &i, &a.screenshot))
                return arg_err("--screenshot needs a value");
        } else if (!strcmp(s, "--full-page"))
            a.full_page = true;
        else if (!strcmp(s, "--verbose") || !strcmp(s, "-v"))
            a.verbose = true;
        else if (!strcmp(s, "--limit")) {
            const char *v;
            long n;
            if (!need_val(argc, argv, &i, &v) || !parse_long(v, 1, 1000000, &n))
                return arg_err("--limit needs a positive number");
            a.limit = (int)n;
        } else if (!strcmp(s, "--depth")) {
            const char *v;
            long n;
            if (!need_val(argc, argv, &i, &v) || !parse_long(v, 0, 100, &n))
                return arg_err("--depth needs a number from 0 to 100");
            a.depth = (int)n;
        } else if (!strcmp(s, "--any-host"))
            a.any_host = true;
        else if (!strcmp(s, "--include")) {
            if (!need_val(argc, argv, &i, &a.include))
                return arg_err("--include needs a value");
        } else if (!strcmp(s, "--exclude")) {
            if (!need_val(argc, argv, &i, &a.exclude))
                return arg_err("--exclude needs a value");
        } else if (!strcmp(s, "--out-dir")) {
            if (!need_val(argc, argv, &i, &a.out_dir))
                return arg_err("--out-dir needs a value");
        } else if (!strcmp(s, "--proxy")) {
            const char *v;
            if (!need_val(argc, argv, &i, &v)) return arg_err("--proxy needs a value");
            if (a.n_proxies >= RBL_MAX_PROXIES) return arg_err("too many --proxy");
            a.proxies[a.n_proxies++] = v;
        } else if (!strcmp(s, "--proxy-strategy")) {
            if (!need_val(argc, argv, &i, &a.proxy_strategy))
                return arg_err("--proxy-strategy needs a value");
        } else if (!strcmp(s, "--profile")) {
            if (!need_val(argc, argv, &i, &a.profile))
                return arg_err("--profile needs a value");
        } else if (!strcmp(s, "--wait-for")) {
            const char *v;
            if (!need_val(argc, argv, &i, &v))
                return arg_err("--wait-for needs a value");
            if (a.n_actions >= RBL_MAX_ACTIONS) return arg_err("too many actions");
            a.actions[a.n_actions++] = (action){ACT_WAIT_FOR, v, 0};
        } else if (!strcmp(s, "--click")) {
            const char *v;
            if (!need_val(argc, argv, &i, &v)) return arg_err("--click needs a value");
            if (a.n_actions >= RBL_MAX_ACTIONS) return arg_err("too many actions");
            a.actions[a.n_actions++] = (action){ACT_CLICK, v, 0};
        } else if (!strcmp(s, "--submit")) {
            const char *v;
            if (!need_val(argc, argv, &i, &v)) return arg_err("--submit needs a value");
            if (a.n_actions >= RBL_MAX_ACTIONS) return arg_err("too many actions");
            a.actions[a.n_actions++] = (action){ACT_SUBMIT, v, 0};
        } else if (!strcmp(s, "--fill")) {
            const char *v;
            if (!need_val(argc, argv, &i, &v)) return arg_err("--fill needs a value");
            if (!strchr(v, '=')) return arg_err("--fill expects selector=value");
            if (a.n_actions >= RBL_MAX_ACTIONS) return arg_err("too many actions");
            /* a holds the full "selector=value"; split at execution. */
            a.actions[a.n_actions++] = (action){ACT_FILL, v, 0};
        } else if (!strcmp(s, "--scroll-bottom")) {
            if (a.n_actions >= RBL_MAX_ACTIONS) return arg_err("too many actions");
            a.actions[a.n_actions++] = (action){ACT_SCROLL_BOTTOM, NULL, 0};
        } else if (!strcmp(s, "--wait")) {
            const char *v;
            long ms;
            if (!need_val(argc, argv, &i, &v) || !parse_long(v, 0, 600000, &ms))
                return arg_err("--wait needs a number of milliseconds");
            if (a.n_actions >= RBL_MAX_ACTIONS) return arg_err("too many actions");
            a.actions[a.n_actions++] = (action){ACT_WAIT, NULL, ms};
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

    rbl_proxy_strategy strat = RBL_PROXY_ROUND_ROBIN;
    if (a.proxy_strategy) {
        if (!strcasecmp(a.proxy_strategy, "random"))
            strat = RBL_PROXY_RANDOM;
        else if (!strcasecmp(a.proxy_strategy, "sticky"))
            strat = RBL_PROXY_STICKY_HOST;
        else if (strcasecmp(a.proxy_strategy, "round-robin"))
            return arg_err("--proxy-strategy must be round-robin, random or sticky");
    }

    rbl_client_config cfg;
    rbl_client_config_init(&cfg);
    if (a.ua) cfg.user_agent = a.ua;
    if (a.lang) cfg.accept_language = a.lang;
    if (a.timeout_ms > 0) cfg.default_timeout_ms = a.timeout_ms;
    if (a.delay_ms > 0) cfg.min_delay_ms = a.delay_ms;
    cfg.allow_private_networks = a.allow_private;
    cfg.browser_profile_dir = a.profile;
    cfg.verbose = a.verbose;

    rbl_client *c = rbl_client_new(&cfg);
    if (!c) {
        fprintf(stderr, "rebellion: out of memory\n");
        return 1;
    }

    if (a.n_proxies > 0 &&
        rbl_client_set_proxies(c, a.proxies, (size_t)a.n_proxies, strat) != RBL_OK) {
        fprintf(stderr, "rebellion: invalid --proxy\n");
        rbl_client_free(c);
        return 2;
    }

    int rc;
    if (!strcmp(a.cmd, "scrape"))
        rc = cmd_scrape(c, &a);
    else if (!strcmp(a.cmd, "crawl"))
        rc = cmd_crawl(c, &a);
    else
        rc = cmd_map(c, &a);

    rbl_client_free(c);
    return rc;
}
