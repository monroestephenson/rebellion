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
#include <stdatomic.h>
#include <stdbool.h>
#include <strings.h>
#include <sys/time.h>
#include <ftw.h>

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

/* --- servers ---------------------------------------------------------- */

/* One small threaded HTTP/1.1 server type, used both as the origin under test
 * and as an authenticating forward proxy. Each connection gets its own thread
 * (Chromium keeps speculative idle connections open, so a serial accept loop
 * would stall), answers one request and closes. */
typedef struct server server;
typedef void (*handler_fn)(server *s, int fd, const char *req);

struct server {
    int lfd;
    int port;
    atomic_int stop;
    atomic_int active; /* connection threads still running */
    pthread_t th;
    handler_fn handler;
    /* proxy mode */
    const char *name;         /* tag rendered into proxied pages        */
    const char *auth;         /* required Proxy-Authorization, or NULL   */
    atomic_int page_hits;     /* forwarded requests for /page           */
    atomic_int auth_failures; /* 407 answers                            */
    atomic_int private_hits;  /* requests aimed at 10.255.255.x         */
};

static void respond(int fd, const char *status, const char *ctype, const char *extra,
                    const char *body, size_t blen) {
    char hdr[1024];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n%s\r\n",
                     status, ctype, blen, extra ? extra : "");
    (void)!write(fd, hdr, (size_t)n);
    if (body && blen) (void)!write(fd, body, blen);
}

static void respond_str(int fd, const char *status, const char *ctype,
                        const char *extra, const char *body) {
    respond(fd, status, ctype, extra, body, strlen(body));
}

/* Split "METHOD target HTTP/1.1" into method and target. */
static bool request_line(const char *req, char *method, size_t mlen, char *target,
                         size_t tlen) {
    const char *sp1 = strchr(req, ' ');
    if (!sp1 || (size_t)(sp1 - req) >= mlen) return false;
    const char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2 || (size_t)(sp2 - sp1 - 1) >= tlen) return false;
    memcpy(method, req, (size_t)(sp1 - req));
    method[sp1 - req] = '\0';
    memcpy(target, sp1 + 1, (size_t)(sp2 - sp1 - 1));
    target[sp2 - sp1 - 1] = '\0';
    return true;
}

/* Copy the value of header `name` (case-insensitive) into out. */
static bool header_value(const char *req, const char *name, char *out, size_t olen) {
    size_t nl = strlen(name);
    for (const char *line = strstr(req, "\r\n"); line; line = strstr(line, "\r\n")) {
        line += 2;
        if (strncasecmp(line, name, nl) == 0 && line[nl] == ':') {
            const char *v = line + nl + 1;
            while (*v == ' ') v++;
            const char *e = strstr(v, "\r\n");
            size_t vl = e ? (size_t)(e - v) : strlen(v);
            if (vl >= olen) vl = olen - 1;
            memcpy(out, v, vl);
            out[vl] = '\0';
            return true;
        }
    }
    return false;
}

typedef struct {
    server *s;
    int fd;
} conn;

static void serve_conn(server *s, int fd) {
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char req[8192];
    size_t got = 0;
    req[0] = '\0';
    while (got < sizeof(req) - 1) {
        ssize_t r = recv(fd, req + got, sizeof(req) - 1 - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (strstr(req, "\r\n\r\n")) s->handler(s, fd, req);
    close(fd);
}

static void *conn_main(void *arg) {
    conn *cn = (conn *)arg;
    serve_conn(cn->s, cn->fd);
    atomic_fetch_sub(&cn->s->active, 1);
    free(cn);
    return NULL;
}

static void *serve(void *arg) {
    server *s = (server *)arg;
    while (!atomic_load(&s->stop)) {
        struct pollfd pfd = {s->lfd, POLLIN, 0};
        if (poll(&pfd, 1, 100) <= 0) continue;
        int fd = accept(s->lfd, NULL, NULL);
        if (fd < 0) continue;
        conn *cn = (conn *)malloc(sizeof(*cn));
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        atomic_fetch_add(&s->active, 1);
        if (cn) {
            cn->s = s;
            cn->fd = fd;
        }
        if (!cn || pthread_create(&t, &at, conn_main, cn) != 0) {
            free(cn);
            serve_conn(s, fd); /* degrade to inline */
            atomic_fetch_sub(&s->active, 1);
        }
        pthread_attr_destroy(&at);
    }
    return NULL;
}

static int server_start(server *s, handler_fn fn, const char *name, const char *auth) {
    memset(s, 0, sizeof(*s));
    s->handler = fn;
    s->name = name;
    s->auth = auth;
    s->lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->lfd < 0) return -1;
    int one = 1;
    setsockopt(s->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    socklen_t al = sizeof(a);
    if (bind(s->lfd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(s->lfd, 64) != 0 ||
        getsockname(s->lfd, (struct sockaddr *)&a, &al) != 0 ||
        pthread_create(&s->th, NULL, serve, s) != 0) {
        close(s->lfd);
        return -1;
    }
    s->port = ntohs(a.sin_port);
    return 0;
}

static void server_stop(server *s) {
    atomic_store(&s->stop, 1);
    pthread_join(s->th, NULL);
    close(s->lfd);
    /* Connection threads time out on idle sockets after 2 s. */
    for (int i = 0; i < 500 && atomic_load(&s->active) > 0; i++) usleep(10000);
}

/* --- the origin ------------------------------------------------------ */

static void origin_handle(server *s, int fd, const char *req) {
    char method[16], target[2048];
    if (!request_line(req, method, sizeof(method), target, sizeof(target))) return;
    char *q = strchr(target, '?');
    if (q) *q = '\0'; /* route on the path */
    const char *path = target;
    char buf[1024];

    if (strcmp(path, "/") == 0 || strcmp(path, "/latin1") == 0) {
        respond(fd, "200 OK", "text/html; charset=ISO-8859-1", NULL, HOME_BODY,
                sizeof(HOME_BODY) - 1);
    } else if (strcmp(path, "/page2") == 0) {
        respond(fd, "200 OK", "text/html; charset=ISO-8859-1", NULL, PAGE2_BODY,
                sizeof(PAGE2_BODY) - 1);
    } else if (strcmp(path, "/interactive") == 0) {
        respond(fd, "200 OK", "text/html; charset=UTF-8", NULL, INTERACTIVE_BODY,
                sizeof(INTERACTIVE_BODY) - 1);
    } else if (strcmp(path, "/robots.txt") == 0) {
        snprintf(buf, sizeof(buf),
                 "User-agent: *\nDisallow: /secret\nDisallow: /*?private=\n"
                 "Sitemap: http://127.0.0.1:%d/sitemap.xml\n",
                 s->port);
        respond_str(fd, "200 OK", "text/plain", NULL, buf);
    } else if (strcmp(path, "/sitemap.xml") == 0) {
        snprintf(buf, sizeof(buf),
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                 "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">"
                 "<url><loc>http://127.0.0.1:%d/from-sitemap</loc></url></urlset>",
                 s->port);
        respond_str(fd, "200 OK", "application/xml", NULL, buf);
    } else if (strcmp(path, "/from-sitemap") == 0) {
        respond(fd, "200 OK", "text/html; charset=UTF-8", NULL, TARGET_BODY,
                sizeof(TARGET_BODY) - 1);
    } else if (strcmp(path, "/secret") == 0) {
        respond_str(fd, "200 OK", "text/html", NULL,
                    "<html><body><p>top secret</p></body></html>");
    } else if (strcmp(path, "/redir") == 0) {
        respond(fd, "301 Moved Permanently", "text/html", "Location: /target\r\n", "",
                0);
    } else if (strcmp(path, "/target") == 0) {
        respond(fd, "200 OK", "text/html; charset=UTF-8", NULL, TARGET_BODY,
                sizeof(TARGET_BODY) - 1);
    } else if (strcmp(path, "/setcookie") == 0) {
        respond_str(fd, "200 OK", "text/html",
                    "Set-Cookie: sid=abc123; Max-Age=3600; Path=/\r\n",
                    "<html><body><p>Cookie gesetzt</p></body></html>");
    } else if (strcmp(path, "/whoami") == 0) {
        char cookie[512] = "";
        header_value(req, "Cookie", cookie, sizeof(cookie));
        snprintf(buf, sizeof(buf), "<html><body><p>COOKIE[%s]</p></body></html>",
                 cookie);
        respond_str(fd, "200 OK", "text/html", NULL, buf);
    } else if (strcmp(path, "/binary") == 0) {
        static const char pdf[] =
            "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n1 0 obj\n<<>>\nendobj\n\0\0\x01";
        respond(fd, "200 OK", "application/pdf", NULL, pdf, sizeof(pdf) - 1);
    } else if (strcmp(path, "/big") == 0) {
        size_t n = 64 * 1024;
        char *big = (char *)malloc(n);
        if (big) {
            memset(big, 'x', n);
            memcpy(big, "<html><body><p>", 15);
            respond(fd, "200 OK", "text/html", NULL, big, n);
            free(big);
        }
    } else {
        respond_str(fd, "404 Not Found", "text/html", NULL,
                    "<html><body>404</body></html>");
    }
}

/* --- the proxy ------------------------------------------------------- */

/* A forward proxy that serves a virtual host, http://example.test, itself
 * (nothing leaves the machine). Requests for any other host are refused and
 * those aimed at the private 10.255.255.0/24 range are counted, so tests can
 * prove the SSRF guard stopped them before they reached the proxy. */
static void proxy_handle(server *s, int fd, const char *req) {
    char method[16], target[2048];
    if (!request_line(req, method, sizeof(method), target, sizeof(target))) return;

    if (strstr(target, "10.255.255.")) atomic_fetch_add(&s->private_hits, 1);

    if (s->auth) {
        char got[256];
        if (!header_value(req, "Proxy-Authorization", got, sizeof(got)) ||
            strcmp(got, s->auth) != 0) {
            atomic_fetch_add(&s->auth_failures, 1);
            respond_str(fd, "407 Proxy Authentication Required", "text/html",
                        "Proxy-Authenticate: Basic realm=\"rebellion-test\"\r\n",
                        "<html><body><p>proxy auth required</p></body></html>");
            return;
        }
    }

    static const char vhost[] = "http://example.test";
    size_t vl = sizeof(vhost) - 1;
    if (strncmp(target, vhost, vl) != 0 || (target[vl] != '/' && target[vl] != '\0')) {
        respond_str(fd, "502 Bad Gateway", "text/plain", NULL, "not proxied here");
        return;
    }
    const char *path = target[vl] ? target + vl : "/";
    char buf[1024];

    if (strcmp(path, "/page") == 0) {
        atomic_fetch_add(&s->page_hits, 1);
        snprintf(
            buf, sizeof(buf),
            "<html lang=\"de\"><head><title>Proxy</title></head><body><article>"
            "<h1>PROXIED-%s</h1><p>Dieser Inhalt wurde vom Test-Proxy ausgeliefert "
            "und ist lang genug, um als Hauptinhalt erkannt zu werden.</p>"
            "</article></body></html>",
            s->name ? s->name : "?");
        respond_str(fd, "200 OK", "text/html; charset=UTF-8", NULL, buf);
    } else if (strcmp(path, "/robots.txt") == 0) {
        respond_str(fd, "200 OK", "text/plain", NULL, "User-agent: *\nAllow: /\n");
    } else if (strcmp(path, "/redirect-private") == 0) {
        respond(fd, "302 Found", "text/html", "Location: http://10.255.255.1/\r\n", "",
                0);
    } else if (strcmp(path, "/link-private") == 0) {
        respond_str(fd, "200 OK", "text/html; charset=UTF-8", NULL,
                    "<html><body><a id=\"go\" href=\"http://10.255.255.2/x\">weiter</a>"
                    "</body></html>");
    } else {
        respond_str(fd, "404 Not Found", "text/html", NULL,
                    "<html><body>404</body></html>");
    }
}

/* "Basic base64(user:pass)" */
#define PROXY_AUTH "Basic dXNlcjpwYXNz"

/* --- the tests ------------------------------------------------------- */

static rbl_client *make_client(void) {
    rbl_client_config cfg;
    rbl_client_config_init(&cfg);
    cfg.allow_private_networks = true; /* we are hitting 127.0.0.1 */
    cfg.min_delay_ms = 0;              /* no need to rate-limit the test */
    return rbl_client_new(&cfg);
}

/* Open a browser session, or report why not. Returns NULL and counts nothing
 * when no Chromium is installed, so CI without a browser still passes. */
static rbl_session *open_session(rbl_client *c, const rbl_session_opts *o,
                                 const char *what) {
    rbl_status st;
    rbl_session *s = rbl_session_open(c, o, &st);
    if (!s) {
        if (st == RBL_ERR_UNSUPPORTED) {
            fprintf(stderr, "  (%s skipped: no Chromium)\n", what);
        } else {
            fprintf(stderr, "FAIL %s: session open failed: %s\n", what,
                    rbl_strerror(st));
            g_tests_run++;
            g_tests_failed++;
        }
    }
    return s;
}

static int rm_entry(const char *path, const struct stat *sb, int flag,
                    struct FTW *ftw) {
    (void)sb;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static void rm_tree(const char *dir) {
    nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* Browser session: fill a field, click, verify DOM mutation, scroll, wait for
 * a lazily-reached element, screenshot. */
static void test_browser_actions(const char *base) {
    rbl_client *c = make_client();
    if (!c) return;
    rbl_session *s = open_session(c, NULL, "browser session test");
    if (!s) {
        rbl_client_free(c);
        return;
    }

    char url[128];
    snprintf(url, sizeof(url), "%s/interactive", base);
    CHECK(rbl_session_goto(s, url, 300) == RBL_OK);
    CHECK(rbl_session_status_code(s) == 200);

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
    CHECK(png != NULL && plen > 8 && memcmp(png, "\x89PNG", 4) == 0);
    free(png);

    rbl_doc *d = rbl_session_capture(s, NULL, true, false);
    CHECK(d != NULL);
    if (d) {
        size_t n = 0;
        CHECK(rbl_doc_screenshot(d, &n) != NULL && n > 8);
        CHECK_STREQ(rbl_doc_engine_used(d), "browser");
        rbl_doc_free(d);
    }

    /* only http(s) navigations */
    CHECK(rbl_session_goto(s, "file:///etc/passwd", 0) == RBL_ERR_BAD_URL);

    /* a 404 page loads but reports its status */
    snprintf(url, sizeof(url), "%s/nope", base);
    CHECK(rbl_session_goto(s, url, 0) == RBL_OK);
    CHECK(rbl_session_status_code(s) == 404);

    rbl_session_close(s);
    rbl_client_free(c);
}

/* Persistent profile: a cookie set in one session is sent by the next one. */
static void test_browser_profile(const char *base) {
    const char *tmp = getenv("TMPDIR");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/rebellion-test-profile-XXXXXX",
             tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(dir)) {
        CHECK(!"mkdtemp");
        return;
    }
    rbl_client_config cfg;
    rbl_client_config_init(&cfg);
    cfg.allow_private_networks = true;
    cfg.browser_profile_dir = dir;
    rbl_client *c = rbl_client_new(&cfg);
    char url[128];

    rbl_session *s = open_session(c, NULL, "persistent profile test");
    if (s) {
        snprintf(url, sizeof(url), "%s/setcookie", base);
        CHECK(rbl_session_goto(s, url, 0) == RBL_OK);

        /* Persistent sessions are serialized: a second one waits, then
         * gives up with BUSY. */
        rbl_session_opts so;
        rbl_session_opts_init(&so);
        so.timeout_ms = 600;
        rbl_status st = RBL_OK;
        rbl_session *other = rbl_session_open(c, &so, &st);
        CHECK(other == NULL);
        CHECK(st == RBL_ERR_BUSY);
        rbl_session_close(other);
        rbl_session_close(s);

        s = open_session(c, NULL, "persistent profile test");
        if (s) {
            snprintf(url, sizeof(url), "%s/whoami", base);
            CHECK(rbl_session_goto(s, url, 0) == RBL_OK);
            char *html = rbl_session_html(s);
            CHECK_CONTAINS(html, "COOKIE[sid=abc123]");
            free(html);
            rbl_session_close(s);
        }
    }
    rbl_client_free(c);
    rm_tree(dir);
}

/* max_concurrent_browsers bounds live Chromium instances. */
static void test_browser_gate(void) {
    rbl_client_config cfg;
    rbl_client_config_init(&cfg);
    cfg.max_concurrent_browsers = 1;
    rbl_client *c = rbl_client_new(&cfg);
    rbl_session *a = open_session(c, NULL, "browser gate test");
    if (a) {
        rbl_session_opts so;
        rbl_session_opts_init(&so);
        so.timeout_ms = 600;
        rbl_status st = RBL_OK;
        rbl_session *b = rbl_session_open(c, &so, &st);
        CHECK(b == NULL);
        CHECK(st == RBL_ERR_BUSY);
        rbl_session_close(b);
        rbl_session_close(a);

        b = rbl_session_open(c, &so, &st); /* the slot is free again */
        CHECK(b != NULL);
        rbl_session_close(b);
    }
    rbl_client_free(c);
}

/* Browser through an authenticating proxy (credentials answered via the
 * DevTools Fetch domain), with the SSRF guard on. */
static void test_browser_proxy(server *px) {
    rbl_client_config cfg;
    rbl_client_config_init(&cfg); /* allow_private_networks = false */
    rbl_client *c = rbl_client_new(&cfg);
    char purl[96];
    snprintf(purl, sizeof(purl), "http://user:pass@127.0.0.1:%d", px->port);
    rbl_session_opts so;
    rbl_session_opts_init(&so);
    so.proxy = purl;
    so.timeout_ms = 20000;
    rbl_session *s = open_session(c, &so, "browser proxy test");
    if (!s) {
        rbl_client_free(c);
        return;
    }

    int hits = atomic_load(&px->page_hits);
    int fails = atomic_load(&px->auth_failures);
    CHECK(rbl_session_goto(s, "http://example.test/page", 0) == RBL_OK);
    CHECK(rbl_session_status_code(s) == 200);
    char *html = rbl_session_html(s);
    CHECK_CONTAINS(html, "PROXIED-eins");
    free(html);
    CHECK(atomic_load(&px->page_hits) > hits);
    CHECK(atomic_load(&px->auth_failures) > fails); /* challenged, then answered */

    /* SSRF: private literal, redirect to a private host, and a click that
     * navigates to one are all stopped before the proxy sees them. */
    CHECK(rbl_session_goto(s, "http://127.0.0.1:9/", 0) == RBL_ERR_BLOCKED);
    CHECK(rbl_session_goto(s, "http://example.test/redirect-private", 0) ==
          RBL_ERR_BLOCKED);
    CHECK(rbl_session_goto(s, "http://example.test/link-private", 0) == RBL_OK);
    CHECK(rbl_session_last_error(s) == RBL_OK);
    CHECK(rbl_session_click(s, "#go") == RBL_OK);
    rbl_session_wait_ms(s, 800);
    CHECK(rbl_session_last_error(s) == RBL_ERR_BLOCKED);
    rbl_doc_free(rbl_session_scrape(s, NULL)); /* must not crash */
    CHECK(atomic_load(&px->private_hits) == 0);

    rbl_session_close(s);
    rbl_client_free(c);
}

/* HTTP engine through a proxy pool: auth, rotation, stickiness, SSRF. */
static void test_proxies(void) {
    server p1, p2;
    if (server_start(&p1, proxy_handle, "eins", PROXY_AUTH) != 0 ||
        server_start(&p2, proxy_handle, "zwei", PROXY_AUTH) != 0) {
        CHECK(!"could not start test proxies");
        return;
    }
    char u1[96], u2[96], bad[96];
    snprintf(u1, sizeof(u1), "http://user:pass@127.0.0.1:%d", p1.port);
    snprintf(u2, sizeof(u2), "http://user:pass@127.0.0.1:%d", p2.port);
    snprintf(bad, sizeof(bad), "http://user:wrong@127.0.0.1:%d", p1.port);
    const char *pool[] = {u1, u2};

    rbl_client_config cfg;
    rbl_client_config_init(&cfg); /* SSRF guard on: the proxy is trusted */
    cfg.min_delay_ms = 0;
    rbl_client *c = rbl_client_new(&cfg);
    rbl_scrape_opts o;
    rbl_scrape_opts_init(&o);
    o.engine = RBL_ENGINE_HTTP;
    o.respect_robots = false;
    rbl_status st;

    /* round robin spreads requests evenly, and credentials are sent */
    CHECK(rbl_client_set_proxies(c, pool, 2, RBL_PROXY_ROUND_ROBIN) == RBL_OK);
    bool all_ok = true;
    for (int i = 0; i < 4; i++) {
        rbl_doc *d = rbl_scrape_ex(c, "http://example.test/page", &o, &st);
        all_ok = all_ok && d && rbl_doc_status_code(d) == 200 &&
                 strstr(rbl_doc_markdown(d), "PROXIED-");
        rbl_doc_free(d);
    }
    CHECK(all_ok);
    CHECK(atomic_load(&p1.page_hits) == 2);
    CHECK(atomic_load(&p2.page_hits) == 2);
    CHECK(atomic_load(&p1.auth_failures) == 0 && atomic_load(&p2.auth_failures) == 0);

    /* sticky-per-host keeps one target host on one proxy */
    CHECK(rbl_client_set_proxies(c, pool, 2, RBL_PROXY_STICKY_HOST) == RBL_OK);
    int h1 = atomic_load(&p1.page_hits), h2 = atomic_load(&p2.page_hits);
    for (int i = 0; i < 3; i++)
        rbl_doc_free(rbl_scrape_ex(c, "http://example.test/page", &o, &st));
    int d1 = atomic_load(&p1.page_hits) - h1, d2 = atomic_load(&p2.page_hits) - h2;
    CHECK((d1 == 3 && d2 == 0) || (d1 == 0 && d2 == 3));

    /* a redirect to a private address is refused, not forwarded */
    rbl_doc *d = rbl_scrape_ex(c, "http://example.test/redirect-private", &o, &st);
    CHECK(d == NULL);
    CHECK(st == RBL_ERR_BLOCKED);
    CHECK(atomic_load(&p1.private_hits) == 0 && atomic_load(&p2.private_hits) == 0);

    /* wrong credentials: the proxy's 407 is surfaced as the status */
    const char *badpool[] = {bad};
    CHECK(rbl_client_set_proxies(c, badpool, 1, RBL_PROXY_ROUND_ROBIN) == RBL_OK);
    d = rbl_scrape_ex(c, "http://example.test/page", &o, &st);
    CHECK(d != NULL && rbl_doc_status_code(d) == 407);
    rbl_doc_free(d);
    CHECK(atomic_load(&p1.auth_failures) > 0);

    /* clearing the pool works */
    CHECK(rbl_client_set_proxies(c, NULL, 0, RBL_PROXY_ROUND_ROBIN) == RBL_OK);
    rbl_client_free(c);

    test_browser_proxy(&p1);

    server_stop(&p1);
    server_stop(&p2);
}

void test_integration(void) {
    signal(SIGPIPE, SIG_IGN);
    server s;
    if (server_start(&s, origin_handle, NULL, NULL) != 0) {
        fprintf(stderr, "FAIL: could not start test server\n");
        g_tests_run++;
        g_tests_failed++;
        return;
    }
    char base[64], url[160];
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

        char *json = rbl_doc_to_json(d, false);
        CHECK_CONTAINS(json, "\"statusCode\":200");
        free(json);
        rbl_doc_free(d);
    }

    /* 2) robots.txt enforced, including query-string rules */
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

    snprintf(url, sizeof(url), "%s/page2?private=1", base);
    d = rbl_scrape_ex(c, url, &o, &st);
    CHECK(d == NULL && st == RBL_ERR_ROBOTS_DENIED);
    snprintf(url, sizeof(url), "%s/page2?public=1", base);
    d = rbl_scrape_ex(c, url, &o, &st);
    CHECK(d != NULL);
    rbl_doc_free(d);

    /* 3) redirects followed to the final URL */
    snprintf(url, sizeof(url), "%s/redir", base);
    d = rbl_scrape_ex(c, url, &o, &st);
    CHECK(d != NULL);
    if (d) {
        CHECK_CONTAINS(rbl_doc_url(d), "/target");
        CHECK_CONTAINS(rbl_doc_markdown(d), "Ziel erreicht");
        rbl_doc_free(d);
    }

    /* 4) error statuses come back as documents; bad input is rejected */
    snprintf(url, sizeof(url), "%s/nope", base);
    d = rbl_scrape_ex(c, url, &o, &st);
    CHECK(d != NULL && rbl_doc_status_code(d) == 404);
    rbl_doc_free(d);

    snprintf(url, sizeof(url), "%s/binary", base);
    d = rbl_scrape_ex(c, url, &o, &st);
    CHECK(d == NULL && st == RBL_ERR_UNSUPPORTED_CONTENT);

    rbl_scrape_opts inj = o;
    inj.user_agent = "rebellion\r\nX-Injected: 1";
    snprintf(url, sizeof(url), "%s/", base);
    d = rbl_scrape_ex(c, url, &inj, &st);
    CHECK(d == NULL && st == RBL_ERR_INVALID_ARG);

    d = rbl_scrape_ex(c, "ftp://127.0.0.1/x", &o, &st);
    CHECK(d == NULL && st == RBL_ERR_BAD_URL);

    /* 5) map: same-host links plus robots.txt Sitemap entries (with port) */
    rbl_strlist list;
    snprintf(url, sizeof(url), "%s/", base);
    st = rbl_map(c, url, 100, &list);
    CHECK(st == RBL_OK);
    bool map_page2 = false, map_sitemap = false;
    char want[160];
    snprintf(want, sizeof(want), "http://127.0.0.1:%d/from-sitemap", s.port);
    for (size_t i = 0; i < list.count; i++) {
        if (strstr(list.items[i], "/page2")) map_page2 = true;
        if (strcmp(list.items[i], want) == 0) map_sitemap = true;
    }
    CHECK(map_page2);
    CHECK(map_sitemap);
    rbl_strlist_free(&list);

    /* 6) crawl walks the site; a robots-denied seed is an error */
    rbl_crawl_opts co;
    rbl_crawl_opts_init(&co);
    co.max_pages = 5;
    co.max_depth = 1;
    co.scrape.engine = RBL_ENGINE_HTTP;
    co.scrape.respect_robots = true;
    int pages = 0;
    extern bool rbl__count_cb(const rbl_doc *, void *);
    CHECK(rbl_crawl(c, url, &co, rbl__count_cb, &pages) == RBL_OK);
    CHECK(pages >= 2); /* home + at least page2 */

    pages = 0;
    snprintf(url, sizeof(url), "%s/secret", base);
    CHECK(rbl_crawl(c, url, &co, rbl__count_cb, &pages) == RBL_ERR_ROBOTS_DENIED);
    CHECK(pages == 0);

    co.include_regex = "([unclosed";
    snprintf(url, sizeof(url), "%s/", base);
    CHECK(rbl_crawl(c, url, &co, rbl__count_cb, &pages) == RBL_ERR_INVALID_ARG);

    rbl_client_free(c);

    /* 7) response size cap */
    rbl_client_config cfg;
    rbl_client_config_init(&cfg);
    cfg.allow_private_networks = true;
    cfg.max_response_bytes = 4096;
    c = rbl_client_new(&cfg);
    snprintf(url, sizeof(url), "%s/big", base);
    d = rbl_scrape_ex(c, url, &o, &st);
    CHECK(d == NULL && st == RBL_ERR_TOO_LARGE);
    rbl_client_free(c);

    /* 8) SSRF guard blocks loopback by default */
    rbl_client_config_init(&cfg);
    c = rbl_client_new(&cfg);
    snprintf(url, sizeof(url), "%s/", base);
    d = rbl_scrape_ex(c, url, &o, &st);
    CHECK(d == NULL && st == RBL_ERR_BLOCKED);
    d = rbl_scrape_ex(c, "http://localhost:9/", &o, &st);
    CHECK(d == NULL && st == RBL_ERR_BLOCKED);
    d = rbl_scrape_ex(c, "http://[::1]:9/", &o, &st);
    CHECK(d == NULL && st == RBL_ERR_BLOCKED);
    rbl_client_free(c);

    /* 9) proxies (HTTP engine + browser) */
    test_proxies();

    /* 10) interactive browser sessions (skipped if no Chromium) */
    test_browser_actions(base);
    test_browser_profile(base);
    test_browser_gate();

    server_stop(&s);
}

bool rbl__count_cb(const rbl_doc *doc, void *user) {
    (void)doc;
    int *n = (int *)user;
    (*n)++;
    return true;
}
