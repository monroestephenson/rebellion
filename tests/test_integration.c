/* Hermetic end-to-end tests: spin up a tiny local HTTP server and drive the
 * public scrape/crawl/map API against it — no network required. */
#include "test.h"
#include "rebellion/rebellion.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>

/* --- canned pages (served verbatim) ---------------------------------- */

/* Homepage: ISO-8859-1. 0xFC = ü, 0xDF = ß, 0xF6 = ö. nav/footer must be
 * stripped; the <article> is the main content. */
static const char HOME_BODY[] =
    "<!DOCTYPE html><html lang=\"de\"><head>"
    "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=ISO-8859-1\">"
    "<title>Startseite</title></head><body>"
    "<nav><a href=\"/menu\">NAVLINK</a></nav>"
    "<article><h1>Gr\xFC\xDF"
    "e aus Hessen</h1>"
    "<p>Willkommen bei der Volksbank. Dies ist ein l\xE4ngerer Absatz mit "
    "ausreichend Text, damit die Hauptinhalts-Heuristik den Artikel sicher "
    "ausw\xE4hlt und die Navigation sowie den Footer zuverl\xE4ssig verwirft. "
    "Besuchen Sie <a href=\"/page2\">unsere zweite Seite</a> oder "
    "<a href=\"https://extern.example/x\">eine externe Seite</a>.</p>"
    "<ul><li>Konten</li><li>Kredite</li></ul></article>"
    "<footer>FOOTERTEXT Impressum</footer></body></html>";

static const char PAGE2_BODY[] =
    "<html lang=\"de\"><head><title>Seite 2</title></head><body>"
    "<main><h1>Zweite Seite</h1><p>Noch mehr Inhalt auf der zweiten Seite, "
    "mit gen\xFCgend Text f\xFCr die Extraktion der Hauptinhalte.</p></main>"
    "</body></html>";

static const char TARGET_BODY[] =
    "<html><head><title>Ziel</title></head><body>"
    "<article><p>Ziel erreicht nach dem Redirect, hier steht der Inhalt "
    "mit ausreichend L\xE4nge f\xFCr die Hauptinhalts-Erkennung.</p></article>"
    "</body></html>";

/* Interactive page for the browser-session test: a form that, on submit,
 * rewrites the heading; plus a tall spacer and a lazily-revealed marker. */
static const char INTERACTIVE_BODY[] =
    "<!DOCTYPE html><html lang=\"de\"><head><title>Interaktiv</title></head>"
    "<body><h1 id=\"title\">START</h1>"
    "<input id=\"q\">"
    "<button id=\"go\" onclick=\"document.getElementById('title').textContent="
    "'TREFFER:'+document.getElementById('q').value\">Suchen</button>"
    "<div style=\"height:3000px\">spacer</div>"
    "<p id=\"deep\">TIEFENINHALT</p></body></html>";

/* --- the server ------------------------------------------------------ */

typedef struct {
    int lfd;
    int port;
    volatile int stop;
    pthread_t th;
} server;

static void send_raw(int fd, const char *s) {
    (void)!write(fd, s, strlen(s));
}

static void respond(int fd, const char *status, const char *ctype, const char *extra,
                    const char *body, size_t blen) {
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n%s\r\n",
                     status, ctype, blen, extra ? extra : "");
    (void)!write(fd, hdr, (size_t)n);
    if (body && blen) (void)!write(fd, body, blen);
}

static void handle(int fd) {
    char req[8192];
    size_t got = 0;
    while (got < sizeof(req) - 1) {
        ssize_t r = recv(fd, req + got, sizeof(req) - 1 - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    /* Parse "GET <path> HTTP/1.1". */
    char path[1024] = "/";
    if (strncmp(req, "GET ", 4) == 0) {
        const char *p = req + 4;
        const char *sp = strchr(p, ' ');
        if (sp && (size_t)(sp - p) < sizeof(path)) {
            memcpy(path, p, (size_t)(sp - p));
            path[sp - p] = '\0';
        }
    }

    if (strcmp(path, "/") == 0) {
        respond(fd, "200 OK", "text/html; charset=ISO-8859-1", NULL, HOME_BODY,
                sizeof(HOME_BODY) - 1);
    } else if (strcmp(path, "/page2") == 0) {
        respond(fd, "200 OK", "text/html; charset=ISO-8859-1", NULL, PAGE2_BODY,
                sizeof(PAGE2_BODY) - 1);
    } else if (strcmp(path, "/latin1") == 0) {
        respond(fd, "200 OK", "text/html; charset=ISO-8859-1", NULL, HOME_BODY,
                sizeof(HOME_BODY) - 1);
    } else if (strcmp(path, "/interactive") == 0) {
        respond(fd, "200 OK", "text/html; charset=UTF-8", NULL, INTERACTIVE_BODY,
                sizeof(INTERACTIVE_BODY) - 1);
    } else if (strcmp(path, "/robots.txt") == 0) {
        const char *r = "User-agent: *\nDisallow: /secret\n";
        respond(fd, "200 OK", "text/plain", NULL, r, strlen(r));
    } else if (strcmp(path, "/secret") == 0) {
        const char *b = "<html><body><p>top secret</p></body></html>";
        respond(fd, "200 OK", "text/html", NULL, b, strlen(b));
    } else if (strcmp(path, "/redir") == 0) {
        respond(fd, "301 Moved Permanently", "text/html", "Location: /target\r\n", "",
                0);
    } else if (strcmp(path, "/target") == 0) {
        respond(fd, "200 OK", "text/html; charset=UTF-8", NULL, TARGET_BODY,
                sizeof(TARGET_BODY) - 1);
    } else {
        const char *b = "<html><body>404</body></html>";
        respond(fd, "404 Not Found", "text/html", NULL, b, strlen(b));
    }
    (void)send_raw;
}

static void *serve(void *arg) {
    server *s = (server *)arg;
    while (!s->stop) {
        struct pollfd pfd = {s->lfd, POLLIN, 0};
        int pr = poll(&pfd, 1, 150);
        if (pr <= 0) continue;
        int fd = accept(s->lfd, NULL, NULL);
        if (fd < 0) continue;
        handle(fd);
        close(fd);
    }
    return NULL;
}

static int server_start(server *s) {
    memset(s, 0, sizeof(*s));
    s->lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->lfd < 0) return -1;
    int one = 1;
    setsockopt(s->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(s->lfd, (struct sockaddr *)&a, sizeof(a)) != 0) return -1;
    if (listen(s->lfd, 16) != 0) return -1;
    socklen_t al = sizeof(a);
    if (getsockname(s->lfd, (struct sockaddr *)&a, &al) != 0) return -1;
    s->port = ntohs(a.sin_port);
    if (pthread_create(&s->th, NULL, serve, s) != 0) return -1;
    return 0;
}

static void server_stop(server *s) {
    s->stop = 1;
    pthread_join(s->th, NULL);
    close(s->lfd);
}

/* --- the tests ------------------------------------------------------- */

static rbl_client *make_client(void) {
    rbl_client_config cfg;
    rbl_client_config_init(&cfg);
    cfg.allow_private_networks = true; /* we are hitting 127.0.0.1 */
    cfg.min_delay_ms = 0;              /* no need to rate-limit the test */
    return rbl_client_new(&cfg);
}

/* Browser session: fill a field, click, verify DOM mutation, scroll, wait for
 * a lazily-reached element, screenshot. Skipped (no failures) when no Chromium
 * is available — so CI without a browser still passes. */
static void test_browser_actions(const char *base) {
    rbl_client_config cfg;
    rbl_client_config_init(&cfg);
    cfg.allow_private_networks = true;
    rbl_client *c = rbl_client_new(&cfg);
    if (!c) return;

    rbl_status st;
    rbl_session *s = rbl_session_open(c, NULL, &st);
    if (!s) {
        if (st == RBL_ERR_UNSUPPORTED)
            fprintf(stderr, "  (browser session test skipped: no Chromium)\n");
        else
            fprintf(stderr, "  (browser session test skipped: %s)\n", rbl_strerror(st));
        rbl_client_free(c);
        return;
    }

    char url[128];
    snprintf(url, sizeof(url), "%s/interactive", base);
    CHECK(rbl_session_goto(s, url, 300) == RBL_OK);

    CHECK(rbl_session_fill(s, "#q", "laptop") == RBL_OK);
    CHECK(rbl_session_click(s, "#go") == RBL_OK);

    char *title = NULL;
    rbl_session_eval(s, "document.getElementById('title').textContent", &title);
    CHECK_STREQ(title, "TREFFER:laptop");
    free(title);

    /* a selector that does not exist fails cleanly */
    CHECK(rbl_session_click(s, "#does-not-exist") == RBL_ERR_PARSE);

    CHECK(rbl_session_scroll_to_bottom(s) == RBL_OK);
    CHECK(rbl_session_wait_for(s, "#deep", 2000) == RBL_OK);
    CHECK(rbl_session_wait_for(s, "#never", 400) == RBL_ERR_TIMEOUT);

    char *html = rbl_session_html(s);
    CHECK(html && strstr(html, "TREFFER:laptop"));
    free(html);

    size_t plen = 0;
    unsigned char *png = rbl_session_screenshot(s, false, &plen);
    CHECK(png != NULL && plen > 8); /* PNG signature + data */
    free(png);

    rbl_session_close(s);
    rbl_client_free(c);
}

void test_integration(void) {
    signal(SIGPIPE, SIG_IGN);
    server s;
    if (server_start(&s) != 0) {
        fprintf(stderr, "FAIL: could not start test server\n");
        g_tests_run++;
        g_tests_failed++;
        return;
    }
    char base[64], url[128];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d", s.port);

    rbl_client *c = make_client();
    CHECK(c != NULL);

    rbl_scrape_opts o;
    rbl_scrape_opts_init(&o);
    o.engine = RBL_ENGINE_HTTP;

    /* 1) homepage: charset, main-content, links, metadata */
    snprintf(url, sizeof(url), "%s/", base);
    rbl_status st;
    rbl_doc *d = rbl_scrape_ex(c, url, &o, &st);
    CHECK(d != NULL);
    if (d) {
        CHECK(rbl_doc_status_code(d) == 200);
        CHECK_STREQ(rbl_doc_engine_used(d), "http");
        CHECK_STREQ(rbl_doc_language(d), "de");
        const char *md = rbl_doc_markdown(d);
        CHECK_CONTAINS(md, "# Grüße aus Hessen"); /* ü ß from Latin-1 */
        CHECK_CONTAINS(md, "längerer");           /* ä from Latin-1  */
        char abs2[160];
        snprintf(abs2, sizeof(abs2), "http://127.0.0.1:%d/page2", s.port);
        CHECK_CONTAINS(md, abs2);                /* resolved link   */
        CHECK(strstr(md, "NAVLINK") == NULL);    /* nav dropped     */
        CHECK(strstr(md, "FOOTERTEXT") == NULL); /* footer dropped  */

        size_t nlinks = 0;
        const char *const *links = rbl_doc_links(d, &nlinks);
        bool found_page2 = false, found_ext = false;
        for (size_t i = 0; i < nlinks; i++) {
            if (strstr(links[i], "/page2")) found_page2 = true;
            if (strstr(links[i], "extern.example")) found_ext = true;
        }
        CHECK(found_page2);
        CHECK(found_ext);
        rbl_doc_free(d);
    }

    /* 2) robots.txt enforced */
    snprintf(url, sizeof(url), "%s/secret", base);
    o.respect_robots = true;
    d = rbl_scrape_ex(c, url, &o, &st);
    CHECK(d == NULL);
    CHECK(st == RBL_ERR_ROBOTS_DENIED);

    o.respect_robots = false; /* explicit override is honored */
    d = rbl_scrape_ex(c, url, &o, &st);
    CHECK(d != NULL);
    if (d) {
        CHECK(rbl_doc_status_code(d) == 200);
        rbl_doc_free(d);
    }
    o.respect_robots = true;

    /* 3) redirects followed to the final URL */
    snprintf(url, sizeof(url), "%s/redir", base);
    d = rbl_scrape_ex(c, url, &o, &st);
    CHECK(d != NULL);
    if (d) {
        CHECK_CONTAINS(rbl_doc_url(d), "/target");
        CHECK_CONTAINS(rbl_doc_markdown(d), "Ziel erreicht");
        rbl_doc_free(d);
    }

    /* 4) map discovers same-host links */
    rbl_strlist list;
    snprintf(url, sizeof(url), "%s/", base);
    st = rbl_map(c, url, 100, &list);
    CHECK(st == RBL_OK);
    bool map_page2 = false;
    for (size_t i = 0; i < list.count; i++)
        if (strstr(list.items[i], "/page2")) map_page2 = true;
    CHECK(map_page2);
    rbl_strlist_free(&list);

    /* 5) crawl walks the site */
    rbl_crawl_opts co;
    rbl_crawl_opts_init(&co);
    co.max_pages = 5;
    co.max_depth = 1;
    co.scrape.engine = RBL_ENGINE_HTTP;
    co.scrape.respect_robots = true;
    int pages = 0;
    extern bool rbl__count_cb(const rbl_doc *, void *);
    rbl_crawl(c, url, &co, rbl__count_cb, &pages);
    CHECK(pages >= 2); /* home + at least page2 */

    rbl_client_free(c);

    /* 6) SSRF guard blocks loopback by default */
    rbl_client_config cfg2;
    rbl_client_config_init(&cfg2);
    cfg2.allow_private_networks = false;
    rbl_client *c2 = rbl_client_new(&cfg2);
    d = rbl_scrape_ex(c2, url, &o, &st);
    CHECK(d == NULL);
    CHECK(st == RBL_ERR_BLOCKED);
    rbl_client_free(c2);

    /* 7) interactive browser session (skipped if no Chromium) */
    test_browser_actions(base);

    server_stop(&s);
}

bool rbl__count_cb(const rbl_doc *doc, void *user) {
    (void)doc;
    int *n = (int *)user;
    (*n)++;
    return true;
}
