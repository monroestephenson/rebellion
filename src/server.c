/*
 * rebellion-server — a small HTTP+JSON API around librebellion, so any
 * service (Kotlin, Go, Python, Node, ...) can scrape/crawl/map over HTTP.
 *
 *   POST /v1/scrape   {"url":"https://example.com", ...}
 *   POST /v1/crawl    {"url":"...", "limit":25, "depth":2, ...}
 *   POST /v1/map      {"url":"...", "limit":1000}
 *   GET  /health
 *
 * One shared, thread-safe rbl_client serves all requests; each connection is
 * handled on its own thread (bounded concurrency). Headless browsers are
 * additionally capped by the client (max_concurrent_browsers).
 */
#include "rebellion/rebellion.h"
#include "jsonval.h"
#include "json.h"
#include "browser.h" /* rbl_browser_available */
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>

#define REQUEST_READ_DEADLINE_MS 30000 /* whole request (anti-slowloris) */
#define MAX_HEADER_BYTES (64 * 1024)

/* ---- config / globals ----------------------------------------------- */

typedef struct {
    rbl_client *client;
    const char *api_key; /* NULL => no auth required */
    size_t max_body;     /* request body cap */
    int crawl_cap;       /* max pages per crawl request */
    int max_conns;       /* concurrent connection cap */
    bool verbose;
    bool quiet;
} srv_cfg;

static srv_cfg g_cfg;
static volatile sig_atomic_t g_stop = 0;
static int g_active_conns = 0;
static pthread_mutex_t g_conn_lock = PTHREAD_MUTEX_INITIALIZER;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* ---- socket helpers ------------------------------------------------- */

static bool send_all(int fd, const void *data, size_t len) {
    const char *p = (const char *)data;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

/* Read more bytes into `in`, honoring the request deadline. */
static bool read_more(int fd, rbl_buf *in, long long deadline) {
    long remain = (long)(deadline - rbl_now_ms());
    if (remain <= 0) return false;
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, (int)remain) <= 0) return false;
    char tmp[16384];
    ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
    if (n <= 0) return false;
    return rbl_buf_append(in, tmp, (size_t)n);
}

static bool ensure_len(int fd, rbl_buf *in, size_t need, long long deadline) {
    while (in->len < need)
        if (!read_more(fd, in, deadline)) return false;
    return true;
}

/* ---- HTTP response -------------------------------------------------- */

static const char *status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 422: return "Unprocessable Content";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Error";
    }
}

static int send_response(int fd, int code, const char *ctype, const char *body,
                         size_t blen) {
    rbl_buf h;
    rbl_buf_init(&h);
    rbl_buf_appendf(&h,
                    "HTTP/1.1 %d %s\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %zu\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                    "Access-Control-Allow-Headers: Content-Type, Authorization, "
                    "X-API-Key\r\n"
                    "Cache-Control: no-store\r\n"
                    "Connection: close\r\n"
                    "Server: rebellion/" RBL_VERSION_STRING "\r\n\r\n",
                    code, status_text(code), ctype, blen);
    send_all(fd, h.data, h.len);
    if (body && blen) send_all(fd, body, blen);
    rbl_buf_free(&h);
    return code;
}

static int send_json(int fd, int code, const char *json) {
    return send_response(fd, code, "application/json; charset=utf-8", json,
                         strlen(json));
}

static int send_error(int fd, int code, const char *errcode, const char *msg) {
    rbl_buf b;
    rbl_buf_init(&b);
    rbl_buf_append_str(&b, "{\"success\":false,\"code\":");
    rbl_json_quote(&b, errcode);
    rbl_buf_append_str(&b, ",\"error\":");
    rbl_json_quote(&b, msg);
    rbl_buf_append_str(&b, "}\n");
    send_json(fd, code, rbl_buf_cstr(&b));
    rbl_buf_free(&b);
    return code;
}

/* Map a library status to an HTTP code + machine-readable code string. */
static int status_http(rbl_status st, const char **code) {
    switch (st) {
    case RBL_OK: *code = "ok"; return 200;
    case RBL_ERR_BAD_URL: *code = "bad_url"; return 400;
    case RBL_ERR_INVALID_ARG: *code = "bad_request"; return 400;
    case RBL_ERR_BLOCKED: *code = "blocked"; return 403;
    case RBL_ERR_ROBOTS_DENIED: *code = "robots_denied"; return 403;
    case RBL_ERR_UNSUPPORTED_CONTENT: *code = "unsupported_content"; return 415;
    case RBL_ERR_TIMEOUT: *code = "timeout"; return 504;
    case RBL_ERR_NETWORK: *code = "network"; return 502;
    case RBL_ERR_TOO_MANY_REDIRECTS: *code = "too_many_redirects"; return 502;
    case RBL_ERR_TOO_LARGE: *code = "response_too_large"; return 502;
    case RBL_ERR_RENDER: *code = "render_failed"; return 502;
    case RBL_ERR_BUSY: *code = "busy"; return 503;
    case RBL_ERR_UNSUPPORTED: *code = "unsupported"; return 501;
    case RBL_ERR_OOM: *code = "out_of_memory"; return 500;
    default: *code = "error"; return 500;
    }
}

static int send_status(int fd, rbl_status st) {
    const char *code;
    int http = status_http(st, &code);
    return send_error(fd, http, code, rbl_strerror(st));
}

/* ---- request -> options -------------------------------------------- */

/* Read a number and clamp it into [lo, hi] *before* converting: a client
 * sending 1e300 must not trigger an out-of-range float->int conversion. */
static long jv_long(const rbl_jv *o, const char *key, long def, long lo, long hi) {
    const rbl_jv *v = rbl_jv_get(o, key);
    if (!v || v->type != RBL_JV_NUM || v->num != v->num) return def;
    if (v->num <= (double)lo) return lo;
    if (v->num >= (double)hi) return hi;
    return (long)v->num;
}

static bool has_ctl(const char *s) {
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c < 0x20 && c != '\t') || c == 0x7F) return true;
    }
    return false;
}

/* Fill scrape options from the request; returns NULL or an error message. */
static const char *opts_from_req(const rbl_jv *req, rbl_scrape_opts *o) {
    rbl_scrape_opts_init(o);
    const char *eng = rbl_jv_get_str(req, "engine", NULL);
    if (eng) {
        if (!strcmp(eng, "http"))
            o->engine = RBL_ENGINE_HTTP;
        else if (!strcmp(eng, "browser"))
            o->engine = RBL_ENGINE_BROWSER;
        else if (!strcmp(eng, "auto"))
            o->engine = RBL_ENGINE_AUTO;
        else
            return "'engine' must be auto, http or browser";
    }
    o->only_main_content = rbl_jv_get_bool(req, "onlyMainContent", true);
    o->include_raw_html = rbl_jv_get_bool(req, "includeHtml", false);
    o->include_links = rbl_jv_get_bool(req, "includeLinks", true);
    o->respect_robots = rbl_jv_get_bool(req, "respectRobots", true);
    o->timeout_ms = jv_long(req, "timeoutMs", 0, 1000, 300000);
    o->wait_after_load_ms = jv_long(req, "waitAfterLoadMs", 800, 0, 60000);
    o->user_agent = rbl_jv_get_str(req, "userAgent", NULL);
    o->accept_language = rbl_jv_get_str(req, "acceptLanguage", NULL);
    if (has_ctl(o->user_agent) || has_ctl(o->accept_language))
        return "userAgent/acceptLanguage must not contain control characters";
    o->screenshot = rbl_jv_get_bool(req, "screenshot", false);
    o->full_page = rbl_jv_get_bool(req, "fullPage", false);
    return NULL;
}

static bool action_known(const char *type, bool *needs_selector) {
    static const char *const with_sel[] = {"click", "fill", "submit", "wait_for", NULL};
    static const char *const without[] = {"scroll_bottom", "scroll", "wait", NULL};
    for (int i = 0; with_sel[i]; i++)
        if (!strcmp(type, with_sel[i])) {
            *needs_selector = true;
            return true;
        }
    for (int i = 0; without[i]; i++)
        if (!strcmp(type, without[i])) {
            *needs_selector = false;
            return true;
        }
    return false;
}

/* Validate "actions" up front so a typo fails fast with a clear 400. */
static const char *validate_actions(const rbl_jv *actions) {
    if (!actions) return NULL;
    if (actions->type != RBL_JV_ARR) return "'actions' must be an array";
    if (actions->count > 50) return "at most 50 actions per request";
    for (size_t i = 0; i < actions->count; i++) {
        const rbl_jv *ac = actions->items[i];
        if (!ac || ac->type != RBL_JV_OBJ) return "each action must be an object";
        const char *type = rbl_jv_get_str(ac, "type", NULL);
        bool needs_sel = false;
        if (!type || !action_known(type, &needs_sel))
            return "unknown action type (use click, fill, submit, wait_for, "
                   "scroll_bottom, scroll or wait)";
        if (needs_sel && !rbl_jv_get_str(ac, "selector", NULL))
            return "this action requires a 'selector'";
    }
    return NULL;
}

/* Run the actions; on a non-optional failure returns its status and index. */
static rbl_status run_actions(rbl_session *s, const rbl_jv *actions, size_t *failed_at) {
    for (size_t i = 0; actions && i < actions->count; i++) {
        const rbl_jv *ac = actions->items[i];
        const char *type = rbl_jv_get_str(ac, "type", "");
        const char *sel = rbl_jv_get_str(ac, "selector", NULL);
        rbl_status st = RBL_OK;
        if (!strcmp(type, "click"))
            st = rbl_session_click(s, sel);
        else if (!strcmp(type, "fill"))
            st = rbl_session_fill(s, sel, rbl_jv_get_str(ac, "value", ""));
        else if (!strcmp(type, "submit"))
            st = rbl_session_submit(s, sel);
        else if (!strcmp(type, "wait_for"))
            st = rbl_session_wait_for(s, sel, jv_long(ac, "ms", 0, 0, 120000));
        else if (!strcmp(type, "scroll_bottom"))
            st = rbl_session_scroll_to_bottom(s);
        else if (!strcmp(type, "scroll"))
            st = rbl_session_scroll(s, (int)jv_long(ac, "x", 0, -100000, 100000),
                                    (int)jv_long(ac, "y", 600, -100000, 100000));
        else if (!strcmp(type, "wait"))
            st = rbl_session_wait_ms(s, jv_long(ac, "ms", 0, 0, 60000));
        if (st == RBL_OK) st = rbl_session_last_error(s); /* blocked navigation */
        if (st != RBL_OK) {
            if (g_cfg.verbose)
                fprintf(stderr, "[server] action %zu (%s) -> %s\n", i, type,
                        rbl_strerror(st));
            if (st == RBL_ERR_BLOCKED || !rbl_jv_get_bool(ac, "optional", false)) {
                *failed_at = i;
                return st;
            }
        }
    }
    return RBL_OK;
}

/* ---- handlers ------------------------------------------------------- */

static int handle_scrape(int fd, const rbl_jv *req) {
    const char *url = rbl_jv_get_str(req, "url", NULL);
    if (!url) return send_error(fd, 400, "bad_request", "missing 'url'");
    rbl_scrape_opts o;
    const char *bad = opts_from_req(req, &o);
    if (bad) return send_error(fd, 400, "bad_request", bad);

    const rbl_jv *actions = rbl_jv_get(req, "actions");
    bad = validate_actions(actions);
    if (bad) return send_error(fd, 400, "bad_request", bad);
    bool have_actions = actions && actions->count > 0;

    rbl_doc *d = NULL;
    rbl_status st = RBL_OK;
    if (have_actions) {
        /* Interactive path: drive a session, then capture. */
        rbl_session_opts so;
        rbl_session_opts_init(&so);
        so.user_agent = o.user_agent;
        so.accept_language = o.accept_language;
        so.respect_robots = o.respect_robots;
        if (o.timeout_ms > 0) so.timeout_ms = o.timeout_ms;
        rbl_session *s = rbl_session_open(g_cfg.client, &so, &st);
        if (!s) return send_status(fd, st);
        st = rbl_session_goto(s, url, o.wait_after_load_ms);
        if (st == RBL_OK) {
            size_t failed_at = 0;
            st = run_actions(s, actions, &failed_at);
            if (st != RBL_OK && st != RBL_ERR_BLOCKED) {
                rbl_session_close(s);
                char msg[160];
                snprintf(msg, sizeof(msg), "action %zu failed: %s", failed_at,
                         rbl_strerror(st));
                return send_error(fd, 422, "action_failed", msg);
            }
        }
        if (st == RBL_OK) {
            d = rbl_session_capture(s, &o, o.screenshot, o.full_page);
            if (!d) st = rbl_session_last_error(s) != RBL_OK ? RBL_ERR_BLOCKED
                                                           : RBL_ERR_RENDER;
        }
        rbl_session_close(s);
    } else {
        d = rbl_scrape_ex(g_cfg.client, url, &o, &st);
    }
    if (!d) return send_status(fd, st);

    char *docjson = rbl_doc_to_json(d, false);
    rbl_doc_free(d);
    if (!docjson) return send_error(fd, 500, "out_of_memory", "serialization failed");
    rbl_buf out;
    rbl_buf_init(&out);
    rbl_buf_append_str(&out, "{\"success\":true,\"data\":");
    rbl_buf_append_str(&out, docjson);
    rbl_buf_append_str(&out, "}\n");
    free(docjson);
    int code = send_json(fd, 200, rbl_buf_cstr(&out));
    rbl_buf_free(&out);
    return code;
}

typedef struct {
    rbl_buf out;
    int count;
    int cap;
    bool first;
} crawl_sink;

static bool crawl_collect(const rbl_doc *doc, void *user) {
    crawl_sink *cs = (crawl_sink *)user;
    char *dj = rbl_doc_to_json(doc, false);
    if (dj) {
        if (!cs->first) rbl_buf_append_str(&cs->out, ",");
        cs->first = false;
        rbl_buf_append_str(&cs->out, dj);
        free(dj);
        cs->count++;
    }
    return cs->count < cs->cap;
}

static int handle_crawl(int fd, const rbl_jv *req) {
    const char *url = rbl_jv_get_str(req, "url", NULL);
    if (!url) return send_error(fd, 400, "bad_request", "missing 'url'");
    rbl_crawl_opts co;
    rbl_crawl_opts_init(&co);
    const rbl_jv *sopt = rbl_jv_get(req, "scrapeOptions");
    const char *bad = opts_from_req(sopt ? sopt : req, &co.scrape);
    if (bad) return send_error(fd, 400, "bad_request", bad);
    int limit = (int)jv_long(req, "limit", 25, 1, g_cfg.crawl_cap);
    co.max_pages = limit;
    co.max_depth = (int)jv_long(req, "depth", 2, 0, 10);
    co.same_host_only = rbl_jv_get_bool(req, "sameHostOnly", true);
    co.allow_subdomains = rbl_jv_get_bool(req, "allowSubdomains", true);
    co.include_regex = rbl_jv_get_str(req, "include", NULL);
    co.exclude_regex = rbl_jv_get_str(req, "exclude", NULL);

    crawl_sink cs;
    rbl_buf_init(&cs.out);
    cs.count = 0;
    cs.cap = limit;
    cs.first = true;
    rbl_buf_append_str(&cs.out, "{\"success\":true,\"data\":[");
    rbl_status st = rbl_crawl(g_cfg.client, url, &co, crawl_collect, &cs);
    if (st != RBL_OK && cs.count == 0) {
        rbl_buf_free(&cs.out);
        return send_status(fd, st);
    }
    rbl_buf_appendf(&cs.out, "],\"count\":%d}\n", cs.count);
    int code = send_json(fd, 200, rbl_buf_cstr(&cs.out));
    rbl_buf_free(&cs.out);
    return code;
}

static int handle_map(int fd, const rbl_jv *req) {
    const char *url = rbl_jv_get_str(req, "url", NULL);
    if (!url) return send_error(fd, 400, "bad_request", "missing 'url'");
    int limit = (int)jv_long(req, "limit", 5000, 1, 100000);
    rbl_strlist list;
    rbl_status st = rbl_map(g_cfg.client, url, limit, &list);
    if (st != RBL_OK) return send_status(fd, st);
    rbl_buf out;
    rbl_buf_init(&out);
    rbl_buf_append_str(&out, "{\"success\":true,\"links\":[");
    for (size_t i = 0; i < list.count; i++) {
        if (i) rbl_buf_append_str(&out, ",");
        rbl_json_quote(&out, list.items[i]);
    }
    rbl_buf_appendf(&out, "],\"count\":%zu}\n", list.count);
    rbl_strlist_free(&list);
    int code = send_json(fd, 200, rbl_buf_cstr(&out));
    rbl_buf_free(&out);
    return code;
}

static int handle_health(int fd) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"service\":\"rebellion\",\"version\":\"%s\","
             "\"browser\":%s}\n",
             RBL_VERSION_STRING, rbl_browser_available(g_cfg.client) ? "true" : "false");
    return send_json(fd, 200, buf);
}

/* ---- request parsing / routing ------------------------------------- */

/* Case-insensitive lookup of a header in the header block (request line
 * first). Returns a heap copy of the trimmed value, or NULL. */
static char *header_value(const char *headers, const char *name) {
    size_t nl = strlen(name);
    const char *p = strstr(headers, "\r\n"); /* skip the request line */
    p = p ? p + 2 : headers;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        if (linelen > nl && strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            const char *e = p + linelen;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
            return rbl_strndup(v, (size_t)(e - v));
        }
        if (!eol) break;
        p = eol + 2;
    }
    return NULL;
}

/* Constant-time comparison, so the API key can't be guessed by timing. */
static bool secret_eq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la != lb);
    for (size_t i = 0; i < la; i++) diff |= (unsigned char)(a[i] ^ b[i % (lb ? lb : 1)]);
    return diff == 0;
}

static bool auth_ok(const char *headers) {
    if (!g_cfg.api_key) return true;
    bool ok = false;
    char *bearer = header_value(headers, "Authorization");
    if (bearer) {
        const char *b = bearer;
        if (strncasecmp(b, "Bearer ", 7) == 0) b += 7;
        ok = secret_eq(b, g_cfg.api_key);
        free(bearer);
    }
    if (!ok) {
        char *k = header_value(headers, "X-API-Key");
        if (k) {
            ok = secret_eq(k, g_cfg.api_key);
            free(k);
        }
    }
    return ok;
}

/* Decode a chunked body starting at in->data[pos]. Returns 0 or an HTTP
 * error code (400/413). */
static int read_chunked(int fd, rbl_buf *in, size_t pos, long long deadline,
                        rbl_buf *body) {
    for (;;) {
        /* chunk-size line */
        char *eol;
        while (!(eol = in->len > pos ? memmem(in->data + pos, in->len - pos, "\r\n", 2)
                                     : NULL)) {
            if (in->len - pos > 1024 || !read_more(fd, in, deadline)) return 400;
        }
        size_t ll = (size_t)(eol - (in->data + pos));
        char line[64];
        if (ll == 0 || ll >= sizeof(line)) return 400;
        memcpy(line, in->data + pos, ll);
        line[ll] = '\0';
        if (!isxdigit((unsigned char)line[0])) return 400;
        char *end = NULL;
        unsigned long long sz = strtoull(line, &end, 16);
        if (*end && *end != ';' && *end != ' ') return 400;
        pos += ll + 2;
        if (sz == 0) { /* trailers, then an empty line */
            for (;;) {
                while (!(eol = in->len > pos
                                   ? memmem(in->data + pos, in->len - pos, "\r\n", 2)
                                   : NULL)) {
                    if (in->len - pos > 8192 || !read_more(fd, in, deadline)) return 400;
                }
                if (eol == in->data + pos) return 0;
                pos = (size_t)(eol - in->data) + 2;
            }
        }
        if (sz > g_cfg.max_body - body->len) return 413;
        if (!ensure_len(fd, in, pos + (size_t)sz + 2, deadline)) return 400;
        rbl_buf_append(body, in->data + pos, (size_t)sz);
        if (memcmp(in->data + pos + sz, "\r\n", 2) != 0) return 400;
        pos += (size_t)sz + 2;
    }
}

static int process_request(int fd, rbl_buf *in, char *method, size_t msz, char *path,
                           size_t psz) {
    long long deadline = rbl_now_ms() + REQUEST_READ_DEADLINE_MS;

    /* Read until end of headers (bounded). */
    size_t header_end = 0;
    for (;;) {
        char *he = in->len >= 4 ? memmem(in->data, in->len, "\r\n\r\n", 4) : NULL;
        if (he) {
            header_end = (size_t)(he - in->data) + 4;
            break;
        }
        if (in->len > MAX_HEADER_BYTES)
            return send_error(fd, 413, "headers_too_large", "request headers too large");
        if (!read_more(fd, in, deadline)) return 0; /* client went away */
    }

    char *hdr = rbl_strndup(in->data, header_end);
    if (!hdr) return 500;
    char rawpath[2048] = {0};
    if (sscanf(hdr, "%15s %2047s", method, rawpath) != 2) {
        free(hdr);
        return send_error(fd, 400, "bad_request", "malformed request line");
    }
    (void)msz;
    /* Route on the path without any query string. */
    char *q = strchr(rawpath, '?');
    if (q) *q = '\0';
    snprintf(path, psz, "%s", rawpath);

    int code;
    if (strcmp(method, "OPTIONS") == 0) {
        code = send_response(fd, 204, "text/plain", "", 0);
        goto out;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        code = handle_health(fd); /* unauthenticated, for load balancers */
        goto out;
    }
    if (!auth_ok(hdr)) {
        code = send_error(fd, 401, "unauthorized", "missing or invalid API key");
        goto out;
    }
    if (strcmp(method, "GET") == 0) {
        code = send_error(fd, 404, "not_found", "unknown endpoint");
        goto out;
    }
    if (strcmp(method, "POST") != 0) {
        code = send_error(fd, 405, "method_not_allowed", "use GET or POST");
        goto out;
    }
    if (strcmp(path, "/v1/scrape") && strcmp(path, "/v1/crawl") &&
        strcmp(path, "/v1/map")) {
        code = send_error(fd, 404, "not_found", "unknown endpoint");
        goto out;
    }

    /* Body: Content-Length or chunked (never both: ambiguous framing). */
    char *cl = header_value(hdr, "Content-Length");
    char *te = header_value(hdr, "Transfer-Encoding");
    char *expect = header_value(hdr, "Expect");
    bool chunked = te && strcasestr(te, "chunked");
    long long clen = -1;
    bool bad_framing = (cl && te);
    if (cl && !bad_framing) {
        char *e = NULL;
        clen = strtoll(cl, &e, 10);
        if (!*cl || *e || clen < 0) bad_framing = true;
    }
    if (te && !chunked) bad_framing = true;
    bool want_continue = expect && strcasecmp(expect, "100-continue") == 0;
    free(cl);
    free(te);
    free(expect);
    if (bad_framing) {
        code = send_error(fd, 400, "bad_request", "invalid message framing");
        goto out;
    }
    if (!chunked && clen < 0) clen = 0;
    if (!chunked && (unsigned long long)clen > g_cfg.max_body) {
        code = send_error(fd, 413, "body_too_large", "request body too large");
        goto out;
    }
    if (want_continue) send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);

    rbl_buf body;
    rbl_buf_init(&body);
    if (chunked) {
        int rc = read_chunked(fd, in, header_end, deadline, &body);
        if (rc) {
            rbl_buf_free(&body);
            code = send_error(fd, rc, rc == 413 ? "body_too_large" : "bad_request",
                              rc == 413 ? "request body too large"
                                        : "malformed chunked body");
            goto out;
        }
    } else {
        if (!ensure_len(fd, in, header_end + (size_t)clen, deadline)) {
            code = send_error(fd, 400, "bad_request", "incomplete request body");
            goto out;
        }
        rbl_buf_append(&body, in->data + header_end, (size_t)clen);
    }

    rbl_jv *req = rbl_jv_parse(body.data ? body.data : "", body.len);
    rbl_buf_free(&body);
    if (!req || req->type != RBL_JV_OBJ) {
        rbl_jv_free(req);
        code = send_error(fd, 400, "bad_json", "request body must be a JSON object");
        goto out;
    }
    if (strcmp(path, "/v1/scrape") == 0)
        code = handle_scrape(fd, req);
    else if (strcmp(path, "/v1/crawl") == 0)
        code = handle_crawl(fd, req);
    else
        code = handle_map(fd, req);
    rbl_jv_free(req);
out:
    free(hdr);
    return code;
}

static void *handle_conn(void *arg) {
    int fd = (int)(intptr_t)arg;
    long long t0 = rbl_now_ms();
    rbl_buf in;
    rbl_buf_init(&in);
    char method[16] = {0}, path[2048] = {0};
    int code = process_request(fd, &in, method, sizeof(method), path, sizeof(path));
    rbl_buf_free(&in);
    close(fd);
    if (!g_cfg.quiet && code)
        fprintf(stderr, "rebellion-server: %s %s %d %lldms\n", method[0] ? method : "-",
                path[0] ? path : "-", code, rbl_now_ms() - t0);
    pthread_mutex_lock(&g_conn_lock);
    g_active_conns--;
    pthread_mutex_unlock(&g_conn_lock);
    return NULL;
}

/* ---- main ----------------------------------------------------------- */

static void usage(void) {
    fputs("rebellion-server " RBL_VERSION_STRING " — HTTP+JSON API for rebellion\n\n"
          "USAGE: rebellion-server [options]\n\n"
          "  --host <addr>          Bind address (default 127.0.0.1; IPv4 or IPv6)\n"
          "  --port <n>             Port (default 8080)\n"
          "  --api-key <key>        Require Authorization: Bearer <key> (or X-API-Key).\n"
          "                         Also read from $REBELLION_API_KEY.\n"
          "  --allow-private        Allow scraping loopback/private/internal hosts\n"
          "  --proxy <url>          Proxy URL; repeat for a rotating pool\n"
          "  --proxy-strategy <s>   round-robin (default) | random | sticky\n"
          "  --profile <dir>        Persistent browser profile directory\n"
          "  --max-browsers <n>     Concurrent headless browsers (default 4)\n"
          "  --crawl-cap <n>        Max pages per crawl request (default 100)\n"
          "  --max-conns <n>        Max concurrent connections (default 32)\n"
          "  --max-body <bytes>     Max request body size (default 2097152)\n"
          "  --quiet                No access log\n"
          "  --verbose, -v          Debug logging to stderr\n"
          "  --help, -h             Show this help\n\n"
          "ENDPOINTS:\n"
          "  GET  /health\n"
          "  POST /v1/scrape   {\"url\":\"https://example.com\"}\n"
          "  POST /v1/crawl    {\"url\":\"...\",\"limit\":25,\"depth\":2}\n"
          "  POST /v1/map      {\"url\":\"...\"}\n",
          stdout);
}

static bool parse_int(const char *s, long lo, long hi, long *out) {
    char *e = NULL;
    long v = strtol(s, &e, 10);
    if (!*s || *e || v < lo || v > hi) return false;
    *out = v;
    return true;
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    long port = 8080, crawl_cap = 100, max_conns = 32, max_browsers = 4;
    long max_body = 2 * 1024 * 1024;
    bool allow_private = false, verbose = false, quiet = false;
    const char *api_key = getenv("REBELLION_API_KEY");
    const char *profile = NULL, *proxy_strategy = NULL;
    const char *proxies[64];
    int n_proxies = 0;

    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        const char *v = NULL;
#define NEED_VAL()                                                                     \
    do {                                                                               \
        if (i + 1 >= argc) {                                                           \
            fprintf(stderr, "rebellion-server: %s needs a value\n", s);                \
            return 2;                                                                  \
        }                                                                              \
        v = argv[++i];                                                                 \
    } while (0)
#define NEED_INT(dst, lo, hi)                                                          \
    do {                                                                               \
        NEED_VAL();                                                                    \
        if (!parse_int(v, lo, hi, &(dst))) {                                           \
            fprintf(stderr, "rebellion-server: invalid value for %s\n", s);            \
            return 2;                                                                  \
        }                                                                              \
    } while (0)
        if (!strcmp(s, "--help") || !strcmp(s, "-h")) {
            usage();
            return 0;
        } else if (!strcmp(s, "--host")) {
            NEED_VAL();
            host = v;
        } else if (!strcmp(s, "--port")) {
            NEED_INT(port, 1, 65535);
        } else if (!strcmp(s, "--api-key")) {
            NEED_VAL();
            api_key = v;
        } else if (!strcmp(s, "--allow-private")) {
            allow_private = true;
        } else if (!strcmp(s, "--proxy")) {
            NEED_VAL();
            if (n_proxies >= 64) {
                fprintf(stderr, "rebellion-server: too many --proxy\n");
                return 2;
            }
            proxies[n_proxies++] = v;
        } else if (!strcmp(s, "--proxy-strategy")) {
            NEED_VAL();
            proxy_strategy = v;
        } else if (!strcmp(s, "--profile")) {
            NEED_VAL();
            profile = v;
        } else if (!strcmp(s, "--max-browsers")) {
            NEED_INT(max_browsers, 1, 256);
        } else if (!strcmp(s, "--crawl-cap")) {
            NEED_INT(crawl_cap, 1, 100000);
        } else if (!strcmp(s, "--max-conns")) {
            NEED_INT(max_conns, 1, 10000);
        } else if (!strcmp(s, "--max-body")) {
            NEED_INT(max_body, 1024, 1L << 30);
        } else if (!strcmp(s, "--quiet")) {
            quiet = true;
        } else if (!strcmp(s, "--verbose") || !strcmp(s, "-v")) {
            verbose = true;
        } else {
            fprintf(stderr, "rebellion-server: unknown option '%s'\n", s);
            return 2;
        }
#undef NEED_INT
#undef NEED_VAL
    }
    if (api_key && !*api_key) api_key = NULL;

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    rbl_client_config cfg;
    rbl_client_config_init(&cfg);
    cfg.allow_private_networks = allow_private;
    cfg.browser_profile_dir = profile;
    cfg.max_concurrent_browsers = (int)max_browsers;
    cfg.verbose = verbose;
    rbl_client *client = rbl_client_new(&cfg);
    if (!client) {
        fprintf(stderr, "rebellion-server: out of memory\n");
        return 1;
    }
    if (n_proxies > 0) {
        rbl_proxy_strategy strat = RBL_PROXY_ROUND_ROBIN;
        if (proxy_strategy) {
            if (!strcasecmp(proxy_strategy, "random"))
                strat = RBL_PROXY_RANDOM;
            else if (!strcasecmp(proxy_strategy, "sticky"))
                strat = RBL_PROXY_STICKY_HOST;
            else if (strcasecmp(proxy_strategy, "round-robin")) {
                fprintf(stderr, "rebellion-server: unknown --proxy-strategy\n");
                return 2;
            }
        }
        rbl_client_set_proxies(client, proxies, (size_t)n_proxies, strat);
    }

    g_cfg.client = client;
    g_cfg.api_key = api_key;
    g_cfg.max_body = (size_t)max_body;
    g_cfg.crawl_cap = (int)crawl_cap;
    g_cfg.max_conns = (int)max_conns;
    g_cfg.verbose = verbose;
    g_cfg.quiet = quiet;

    /* Bind (IPv4 or IPv6) via getaddrinfo. */
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%ld", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "rebellion-server: invalid --host '%s': %s\n", host,
                gai_strerror(gai));
        return 1;
    }
    int lfd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        lfd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (lfd < 0) continue;
        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(lfd, a->ai_addr, a->ai_addrlen) == 0 && listen(lfd, 128) == 0) break;
        close(lfd);
        lfd = -1;
    }
    freeaddrinfo(res);
    if (lfd < 0) {
        fprintf(stderr, "rebellion-server: cannot listen on %s:%ld: %s\n", host, port,
                strerror(errno));
        return 1;
    }

    fprintf(stderr,
            "rebellion-server %s listening on %s:%ld (api key: %s, browser: %s, "
            "max browsers: %ld)\n",
            RBL_VERSION_STRING, host, port, api_key ? "required" : "none",
            rbl_browser_available(client) ? "available" : "unavailable", max_browsers);
    if (!api_key && strcmp(host, "127.0.0.1") && strcmp(host, "::1") &&
        strcmp(host, "localhost"))
        fprintf(stderr, "rebellion-server: WARNING: listening on a non-loopback "
                        "address without --api-key\n");

    while (!g_stop) {
        struct pollfd pfd = {lfd, POLLIN, 0};
        int pr = poll(&pfd, 1, 500);
        if (pr <= 0) continue;
        int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        if (cfd < 0) continue;
        int nodelay = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        pthread_mutex_lock(&g_conn_lock);
        bool over = g_active_conns >= g_cfg.max_conns;
        if (!over) g_active_conns++;
        pthread_mutex_unlock(&g_conn_lock);
        if (over) {
            send_error(cfd, 503, "busy", "too many concurrent connections");
            close(cfd);
            continue;
        }
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &attr, handle_conn, (void *)(intptr_t)cfd) != 0) {
            pthread_mutex_lock(&g_conn_lock);
            g_active_conns--;
            pthread_mutex_unlock(&g_conn_lock);
            close(cfd);
        }
        pthread_attr_destroy(&attr);
    }

    /* Stop accepting, then let in-flight requests finish before freeing the
     * shared client (freeing it under a running request would be a
     * use-after-free). */
    close(lfd);
    fprintf(stderr, "rebellion-server: shutting down, draining requests\n");
    long long deadline = rbl_now_ms() + 60000;
    for (;;) {
        pthread_mutex_lock(&g_conn_lock);
        int active = g_active_conns;
        pthread_mutex_unlock(&g_conn_lock);
        if (active == 0) break;
        if (rbl_now_ms() > deadline) {
            fprintf(stderr, "rebellion-server: %d request(s) still running; exiting\n",
                    active);
            return 0; /* skip freeing: threads may still use the client */
        }
        rbl_sleep_ms(100);
    }
    rbl_client_free(client);
    return 0;
}
