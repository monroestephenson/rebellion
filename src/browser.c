#include "browser.h"
#include "internal.h"
#include "json.h"
#include "net.h"
#include "robots.h"
#include "url.h"
#include "util.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <ftw.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <time.h>

bool rbl_browser_available(rbl_client *c) {
    return c && c->chromium_path != NULL;
}

/* ---- tiny TCP + buffered reader ------------------------------------- */

typedef struct {
    int fd;
    unsigned char buf[16384];
    size_t start, end;
} sockrd;

/* Connect to 127.0.0.1:port, retrying until timeout. A fresh socket is used
 * per attempt: after a failed connect() the old one is in an unspecified
 * state. */
static int tcp_connect_local(int port, long timeout_ms) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    long long deadline = rbl_now_ms() + timeout_ms;
    for (;;) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            return fd;
        }
        close(fd);
        if (rbl_now_ms() > deadline) return -1;
        rbl_sleep_ms(50);
    }
}

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

static void sockrd_init(sockrd *r, int fd) {
    r->fd = fd;
    r->start = r->end = 0;
}

/* Read at least one byte into the buffer; returns bytes available or <=0. */
static int sockrd_fill(sockrd *r, long timeout_ms) {
    if (r->start < r->end) return (int)(r->end - r->start);
    r->start = r->end = 0;
    struct pollfd pfd = {r->fd, POLLIN, 0};
    int pr = poll(&pfd, 1, (int)timeout_ms);
    if (pr <= 0) return pr; /* 0 = timeout, <0 = error */
    ssize_t n = recv(r->fd, r->buf, sizeof(r->buf), 0);
    if (n <= 0) return -1;
    r->end = (size_t)n;
    return (int)n;
}

static int sockrd_getc(sockrd *r, long timeout_ms) {
    if (r->start >= r->end) {
        int f = sockrd_fill(r, timeout_ms);
        if (f <= 0) return -1;
    }
    return r->buf[r->start++];
}

static bool sockrd_read_n(sockrd *r, unsigned char *out, size_t n, long timeout_ms) {
    size_t got = 0;
    long long deadline = rbl_now_ms() + timeout_ms;
    while (got < n) {
        if (r->start >= r->end) {
            long remain = (long)(deadline - rbl_now_ms());
            if (remain <= 0) return false;
            int f = sockrd_fill(r, remain);
            if (f <= 0) return false;
        }
        size_t avail = r->end - r->start;
        size_t take = (n - got < avail) ? (n - got) : avail;
        memcpy(out + got, r->buf + r->start, take);
        r->start += take;
        got += take;
    }
    return true;
}

/* ---- local HTTP (DevTools JSON endpoints) --------------------------- */

/* GET a DevTools JSON endpoint; stops at Content-Length rather than waiting
 * for the server to close the connection. Returns the body or NULL. */
static char *http_local_get(int port, const char *path, long timeout_ms) {
    int fd = tcp_connect_local(port, timeout_ms);
    if (fd < 0) return NULL;
    rbl_buf req;
    rbl_buf_init(&req);
    rbl_buf_appendf(&req,
                    "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
                    "Accept: application/json\r\nConnection: close\r\n\r\n",
                    path, port);
    bool ok = send_all(fd, req.data, req.len);
    rbl_buf_free(&req);
    if (!ok) {
        close(fd);
        return NULL;
    }

    sockrd r;
    sockrd_init(&r, fd);
    rbl_buf resp;
    rbl_buf_init(&resp);
    long long deadline = rbl_now_ms() + timeout_ms;
    long clen = -1;
    size_t body_off = 0;
    for (;;) {
        long remain = (long)(deadline - rbl_now_ms());
        if (remain <= 0) break;
        int f = sockrd_fill(&r, remain);
        if (f <= 0) break;
        rbl_buf_append(&resp, r.buf + r.start, r.end - r.start);
        r.start = r.end;
        char *s = rbl_buf_cstr(&resp);
        if (!body_off) {
            char *he = strstr(s, "\r\n\r\n");
            if (he) {
                body_off = (size_t)(he - s) + 4;
                char *cl = strcasestr(s, "\r\nContent-Length:");
                if (cl && cl < he) clen = atol(cl + 17);
            }
        }
        if (body_off && clen >= 0 && resp.len - body_off >= (size_t)clen) break;
    }
    close(fd);

    char *out = NULL;
    if (body_off) {
        size_t blen = resp.len - body_off;
        if (clen >= 0 && (size_t)clen < blen) blen = (size_t)clen;
        out = rbl_strndup(resp.data + body_off, blen);
    }
    rbl_buf_free(&resp);
    return out;
}

/* ---- WebSocket (client) --------------------------------------------- */

/* Request interception state (Fetch domain). Used for proxy authentication
 * and to stop navigations to private hosts (SSRF guard). */
typedef struct {
    bool enabled;
    bool allow_private;
    bool check_dns;   /* resolve hostnames: only meaningful without a proxy */
    char *proxy_user; /* owned */
    char *proxy_pass; /* owned */
    char *main_frame; /* owned: main frame id */
    bool blocked_main;
    int auth_attempts;
} intercept_ctx;

typedef struct {
    int fd;
    sockrd rd;
    int next_id;
    intercept_ctx ic;
} ws_conn;

static void ws_mask_key(unsigned char k[4]) {
    for (int i = 0; i < 4; i++) k[i] = (unsigned char)(rand() & 0xFF);
}

/* Parse ws://host:port/path from a url; returns heap path and fills port. */
static char *ws_parse(const char *wsurl, int *port) {
    const char *p = wsurl;
    if (strncmp(p, "ws://", 5) == 0)
        p += 5;
    else if (strncmp(p, "wss://", 6) == 0)
        p += 6;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) *port = atoi(colon + 1);
    return rbl_strdup(slash ? slash : "/");
}

static bool ws_handshake(ws_conn *w, int port, const char *path, long timeout_ms) {
    /* A fixed key is fine: this only ever talks to the local DevTools port. */
    const char *key = "x3JJHMbDL1EzLkh9GBhXDw==";
    rbl_buf req;
    rbl_buf_init(&req);
    rbl_buf_appendf(&req,
                    "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
                    "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                    "Sec-WebSocket-Version: 13\r\n\r\n",
                    path, port, key);
    bool ok = send_all(w->fd, req.data, req.len);
    rbl_buf_free(&req);
    if (!ok) return false;

    /* Read status line + headers up to the blank line. */
    int nl_state = 0;
    bool is101 = false, first_line = true;
    rbl_buf statusline;
    rbl_buf_init(&statusline);
    for (;;) {
        int ch = sockrd_getc(&w->rd, timeout_ms);
        if (ch < 0) {
            rbl_buf_free(&statusline);
            return false;
        }
        if (ch == '\r') continue;
        if (ch == '\n') {
            if (first_line) {
                char *sl = rbl_buf_cstr(&statusline);
                if (sl && strstr(sl, " 101")) is101 = true;
                first_line = false;
            }
            if (++nl_state == 2) break;
            continue;
        }
        nl_state = 0;
        if (first_line) rbl_buf_append_ch(&statusline, (char)ch);
    }
    rbl_buf_free(&statusline);
    return is101;
}

static bool ws_send_text(ws_conn *w, const char *data, size_t len) {
    unsigned char hdr[14];
    size_t hn = 0;
    hdr[hn++] = 0x81; /* FIN + text */
    if (len < 126) {
        hdr[hn++] = 0x80 | (unsigned char)len;
    } else if (len <= 0xFFFF) {
        hdr[hn++] = 0x80 | 126;
        hdr[hn++] = (unsigned char)(len >> 8);
        hdr[hn++] = (unsigned char)(len & 0xFF);
    } else {
        hdr[hn++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--)
            hdr[hn++] = (unsigned char)(((uint64_t)len >> (i * 8)) & 0xFF);
    }
    unsigned char mk[4];
    ws_mask_key(mk);
    memcpy(hdr + hn, mk, 4);
    hn += 4;
    if (!send_all(w->fd, hdr, hn)) return false;

    unsigned char tmp[4096];
    size_t i = 0;
    while (i < len) {
        size_t chunk = (len - i < sizeof(tmp)) ? (len - i) : sizeof(tmp);
        for (size_t j = 0; j < chunk; j++)
            tmp[j] = (unsigned char)data[i + j] ^ mk[(i + j) & 3];
        if (!send_all(w->fd, tmp, chunk)) return false;
        i += chunk;
    }
    return true;
}

/* Receive one complete text message (handling fragmentation + control
 * frames). Returns a heap string or NULL on error/timeout. */
static char *ws_recv_text(ws_conn *w, long timeout_ms) {
    rbl_buf msg;
    rbl_buf_init(&msg);
    long long deadline = rbl_now_ms() + timeout_ms;
    for (;;) {
        long remain = (long)(deadline - rbl_now_ms());
        if (remain <= 0) goto fail;

        unsigned char h2[2];
        if (!sockrd_read_n(&w->rd, h2, 2, remain)) goto fail;
        int fin = (h2[0] & 0x80) != 0;
        int opcode = h2[0] & 0x0F;
        int masked = (h2[1] & 0x80) != 0;
        uint64_t plen = h2[1] & 0x7F;
        if (plen == 126) {
            unsigned char e[2];
            if (!sockrd_read_n(&w->rd, e, 2, remain)) goto fail;
            plen = ((uint64_t)e[0] << 8) | e[1];
        } else if (plen == 127) {
            unsigned char e[8];
            if (!sockrd_read_n(&w->rd, e, 8, remain)) goto fail;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | e[i];
        }
        unsigned char mk[4] = {0, 0, 0, 0};
        if (masked && !sockrd_read_n(&w->rd, mk, 4, remain)) goto fail;

        /* Cap message size to avoid runaway allocations (256 MiB). */
        if (plen > (uint64_t)256 * 1024 * 1024 ||
            msg.len + plen > (uint64_t)256 * 1024 * 1024)
            goto fail;
        unsigned char *payload = (unsigned char *)malloc((size_t)plen + 1);
        if (!payload) goto fail;
        if (plen && !sockrd_read_n(&w->rd, payload, (size_t)plen, remain)) {
            free(payload);
            goto fail;
        }
        if (masked)
            for (uint64_t i = 0; i < plen; i++) payload[i] ^= mk[i & 3];

        if (opcode == 0x8) { /* close */
            free(payload);
            goto fail;
        } else if (opcode == 0x9) { /* ping -> (empty, masked) pong */
            unsigned char pong[6] = {0x8A, 0x80, 0, 0, 0, 0};
            send_all(w->fd, pong, sizeof(pong));
            free(payload);
            continue;
        } else if (opcode == 0xA) { /* pong */
            free(payload);
            continue;
        }
        rbl_buf_append(&msg, payload, (size_t)plen);
        free(payload);
        if (fin) break;
    }
    return rbl_buf_steal(&msg);
fail:
    rbl_buf_free(&msg);
    return NULL;
}

/* ---- CDP ------------------------------------------------------------ */

/* Top-level "id" of a CDP response, or -1 for events. Responses are
 * serialized as {"id":N,...}; matching the leading key exactly avoids
 * confusing them with ids nested inside events (e.g. execution contexts). */
static long msg_id(const char *msg) {
    const char *p = msg;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p != '{') return -1;
    p++;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (strncmp(p, "\"id\":", 5) != 0) return -1;
    char *end = NULL;
    long id = strtol(p + 5, &end, 10);
    return end == p + 5 ? -1 : id;
}

/* Decoded string value of `key`, searched after `anchor` if given. */
static char *json_str(const char *msg, const char *anchor, const char *key) {
    const char *from = msg;
    if (anchor) {
        from = strstr(msg, anchor);
        if (!from) return NULL;
    }
    const char *v = rbl_json_find_value(from, key);
    return (v && *v == '"') ? rbl_json_decode_string(v, NULL) : NULL;
}

/* Send a command without waiting; returns its id or -1. params may be NULL. */
static int cdp_send(ws_conn *w, const char *method, const char *params) {
    int id = ++w->next_id;
    rbl_buf cmd;
    rbl_buf_init(&cmd);
    rbl_buf_appendf(&cmd, "{\"id\":%d,\"method\":\"%s\"", id, method);
    if (params) rbl_buf_appendf(&cmd, ",\"params\":%s", params);
    rbl_buf_append_ch(&cmd, '}');
    bool ok = ws_send_text(w, cmd.data, cmd.len);
    rbl_buf_free(&cmd);
    return ok ? id : -1;
}

static void handle_request_paused(ws_conn *w, const char *msg) {
    intercept_ctx *ic = &w->ic;
    char *rid = json_str(msg, NULL, "requestId");
    if (!rid) return;
    char *url = json_str(msg, "\"request\"", "url");
    char *rtype = json_str(msg, NULL, "resourceType");
    char *frame = json_str(msg, NULL, "frameId");

    bool block = false;
    if (!ic->allow_private && url) {
        rbl_url u;
        if (rbl_url_parse(url, &u)) { /* http(s) only; data:/blob: have no host */
            block = ic->check_dns ? rbl_host_resolves_private(u.host)
                                  : rbl_host_is_private(u.host);
            rbl_url_free(&u);
        }
    }

    rbl_buf p;
    rbl_buf_init(&p);
    rbl_buf_append_str(&p, "{\"requestId\":");
    rbl_json_quote(&p, rid);
    if (block) {
        rbl_logf("SSRF guard: blocking browser request to %s", url);
        rbl_buf_append_str(&p, ",\"errorReason\":\"BlockedByClient\"}");
        cdp_send(w, "Fetch.failRequest", rbl_buf_cstr(&p));
        bool is_doc = rtype && strcmp(rtype, "Document") == 0;
        bool is_main = !ic->main_frame || (frame && strcmp(frame, ic->main_frame) == 0);
        if (is_doc && is_main) ic->blocked_main = true;
    } else {
        rbl_buf_append_ch(&p, '}');
        cdp_send(w, "Fetch.continueRequest", rbl_buf_cstr(&p));
    }
    rbl_buf_free(&p);
    free(rid);
    free(url);
    free(rtype);
    free(frame);
}

static void handle_auth_required(ws_conn *w, const char *msg) {
    intercept_ctx *ic = &w->ic;
    char *rid = json_str(msg, NULL, "requestId");
    if (!rid) return;
    char *source = json_str(msg, "\"authChallenge\"", "source");
    bool provide = source && strcmp(source, "Proxy") == 0 && ic->proxy_user &&
                   ic->auth_attempts < 3;
    rbl_buf p;
    rbl_buf_init(&p);
    rbl_buf_append_str(&p, "{\"requestId\":");
    rbl_json_quote(&p, rid);
    if (provide) {
        ic->auth_attempts++;
        rbl_buf_append_str(&p, ",\"authChallengeResponse\":{\"response\":"
                               "\"ProvideCredentials\",\"username\":");
        rbl_json_quote(&p, ic->proxy_user);
        rbl_buf_append_str(&p, ",\"password\":");
        rbl_json_quote(&p, ic->proxy_pass ? ic->proxy_pass : "");
        rbl_buf_append_str(&p, "}}");
    } else {
        /* Server auth challenges (and repeated proxy failures) are declined. */
        rbl_buf_append_str(&p, ",\"authChallengeResponse\":{\"response\":"
                               "\"CancelAuth\"}}");
    }
    cdp_send(w, "Fetch.continueWithAuth", rbl_buf_cstr(&p));
    rbl_buf_free(&p);
    free(rid);
    free(source);
}

static void handle_event(ws_conn *w, const char *msg) {
    if (!w->ic.enabled) return;
    if (strstr(msg, "\"method\":\"Fetch.requestPaused\""))
        handle_request_paused(w, msg);
    else if (strstr(msg, "\"method\":\"Fetch.authRequired\""))
        handle_auth_required(w, msg);
}

/* Return the next command response, servicing events on the way. NULL on
 * timeout or a dead connection. */
static char *cdp_recv(ws_conn *w, long timeout_ms) {
    long long deadline = rbl_now_ms() + timeout_ms;
    for (;;) {
        long remain = (long)(deadline - rbl_now_ms());
        if (remain <= 0) return NULL;
        char *msg = ws_recv_text(w, remain);
        if (!msg) return NULL;
        if (msg_id(msg) >= 0) return msg;
        handle_event(w, msg);
        free(msg);
    }
}

/* Send a command and wait for its response (caller frees), or NULL. */
static char *cdp_call(ws_conn *w, const char *method, const char *params,
                      long timeout_ms) {
    int id = cdp_send(w, method, params);
    if (id < 0) return NULL;
    long long deadline = rbl_now_ms() + timeout_ms;
    for (;;) {
        long remain = (long)(deadline - rbl_now_ms());
        if (remain <= 0) return NULL;
        char *msg = cdp_recv(w, remain);
        if (!msg) return NULL;
        if (msg_id(msg) == id) return msg;
        free(msg); /* late reply to an earlier fire-and-forget command */
    }
}

/* Keep servicing events for `ms` (instead of sleeping), so intercepted
 * requests are never left paused while we wait. */
static void cdp_pump(ws_conn *w, long ms) {
    long long deadline = rbl_now_ms() + ms;
    for (;;) {
        long remain = (long)(deadline - rbl_now_ms());
        if (remain <= 0) return;
        char *msg = cdp_recv(w, remain);
        if (!msg) {
            /* Timeout, or a dead socket: avoid spinning on the latter. */
            long left = (long)(deadline - rbl_now_ms());
            if (left > 0) rbl_sleep_ms(left < 50 ? left : 50);
            continue;
        }
        free(msg);
    }
}

static bool cdp_call_ok(ws_conn *w, const char *method, const char *params,
                        long timeout_ms) {
    char *r = cdp_call(w, method, params, timeout_ms);
    bool ok = r && !strstr(r, "\"error\":{");
    free(r);
    return ok;
}

/* Runtime.evaluate returning the string value, or NULL. */
static char *cdp_eval_string(ws_conn *w, const char *expr, long timeout_ms) {
    rbl_buf p;
    rbl_buf_init(&p);
    rbl_buf_append_str(&p, "{\"expression\":");
    rbl_json_quote(&p, expr);
    rbl_buf_append_str(&p, ",\"returnByValue\":true,\"awaitPromise\":true}");
    char *msg = cdp_call(w, "Runtime.evaluate", rbl_buf_cstr(&p), timeout_ms);
    rbl_buf_free(&p);
    if (!msg) return NULL;
    /* {"id":N,"result":{"result":{"type":"string","value":"..."}}} */
    char *out = NULL;
    if (!strstr(msg, "\"exceptionDetails\"")) out = json_str(msg, "\"result\"", "value");
    free(msg);
    return out;
}

/* Map a Page.navigate errorText to a status. */
static rbl_status nav_error_status(const char *err) {
    if (strstr(err, "BLOCKED_BY_CLIENT")) return RBL_ERR_BLOCKED;
    if (strstr(err, "TIMED_OUT")) return RBL_ERR_TIMEOUT;
    if (strstr(err, "ERR_ABORTED")) return RBL_ERR_UNSUPPORTED_CONTENT; /* download */
    return RBL_ERR_NETWORK;
}

static rbl_status cdp_navigate(ws_conn *w, const char *url, long timeout_ms) {
    rbl_buf p;
    rbl_buf_init(&p);
    rbl_buf_append_str(&p, "{\"url\":");
    rbl_json_quote(&p, url);
    rbl_buf_append_ch(&p, '}');
    char *msg = cdp_call(w, "Page.navigate", rbl_buf_cstr(&p), timeout_ms);
    rbl_buf_free(&p);
    if (!msg) return RBL_ERR_TIMEOUT;
    rbl_status st = RBL_OK;
    char *err = json_str(msg, NULL, "errorText");
    if (err && *err) {
        rbl_logf("browser: navigation to %s failed: %s", url, err);
        st = nav_error_status(err);
    } else if (strstr(msg, "\"error\":{")) {
        st = RBL_ERR_BAD_URL;
    }
    free(err);
    free(msg);
    return st;
}

static unsigned char *cdp_screenshot(ws_conn *w, bool full_page, long timeout_ms,
                                     size_t *out_len) {
    char params[128];
    snprintf(params, sizeof(params),
             "{\"format\":\"png\",\"captureBeyondViewport\":%s,\"fromSurface\":true}",
             full_page ? "true" : "false");
    char *msg = cdp_call(w, "Page.captureScreenshot", params, timeout_ms);
    if (!msg) return NULL;
    char *b64 = json_str(msg, NULL, "data");
    free(msg);
    if (!b64) return NULL;
    unsigned char *png = rbl_base64_decode(b64, strlen(b64), out_len);
    free(b64);
    return png;
}

/* ---- proxies -------------------------------------------------------- */

/* Split a curl-style proxy URL into what Chromium accepts on --proxy-server
 * ("scheme://host:port", no credentials) plus decoded credentials. Chromium
 * ignores credentials embedded in --proxy-server, so they are supplied via
 * Fetch.authRequired instead. Returns false if it cannot be parsed. */
static bool split_proxy(const char *in, char **chrome_proxy, char **user, char **pass) {
    *chrome_proxy = *user = *pass = NULL;
    CURLU *h = curl_url();
    if (!h) return false;
    bool ok = false;
    char *scheme = NULL, *host = NULL, *port = NULL, *u = NULL, *pw = NULL;
    if (curl_url_set(h, CURLUPART_URL, in, CURLU_NON_SUPPORT_SCHEME | CURLU_GUESS_SCHEME) !=
        CURLUE_OK)
        goto done;
    curl_url_get(h, CURLUPART_SCHEME, &scheme, 0);
    curl_url_get(h, CURLUPART_HOST, &host, 0);
    curl_url_get(h, CURLUPART_PORT, &port, 0);
    curl_url_get(h, CURLUPART_USER, &u, CURLU_URLDECODE);
    curl_url_get(h, CURLUPART_PASSWORD, &pw, CURLU_URLDECODE);
    if (!scheme || !host) goto done;

    const char *cs = "http";
    if (!strcasecmp(scheme, "https"))
        cs = "https";
    else if (!strcasecmp(scheme, "socks5") || !strcasecmp(scheme, "socks5h"))
        cs = "socks5";
    else if (!strcasecmp(scheme, "socks4") || !strcasecmp(scheme, "socks4a"))
        cs = "socks4";
    if (u && *u && !strncmp(cs, "socks", 5))
        rbl_logf("browser: Chromium cannot authenticate to SOCKS proxies; "
                 "credentials for %s are ignored by the browser engine",
                 host);

    rbl_buf b;
    rbl_buf_init(&b);
    rbl_buf_appendf(&b, "%s://%s", cs, host);
    if (port) rbl_buf_appendf(&b, ":%s", port);
    *chrome_proxy = rbl_buf_steal(&b);
    if (u && *u) {
        *user = rbl_strdup(u);
        *pass = rbl_strdup(pw ? pw : "");
    }
    ok = *chrome_proxy != NULL;
done:
    curl_free(scheme);
    curl_free(host);
    curl_free(port);
    curl_free(u);
    curl_free(pw);
    curl_url_cleanup(h);
    return ok;
}

/* ---- process launch ------------------------------------------------- */

typedef struct {
    pid_t pid;
    char profile_dir[512];
    bool owns_profile; /* true => created a throwaway dir, delete on close */
} browser_proc;

typedef struct {
    const char *ua;
    const char *al;
    const char *proxy;       /* Chromium form (no credentials), or NULL */
    const char *profile_dir; /* NULL => throwaway temp profile          */
} launch_opts;

static void rm_rf(const char *path); /* fwd */

static bool launch_chromium(rbl_client *c, const launch_opts *o, browser_proc *bp) {
    if (o->profile_dir && *o->profile_dir) {
        /* Persistent profile: create if needed, keep it on close. A port file
         * left behind by a killed browser would be read before the new one is
         * written, so remove it first. */
        mkdir(o->profile_dir, 0700);
        snprintf(bp->profile_dir, sizeof(bp->profile_dir), "%s", o->profile_dir);
        char pf[600];
        snprintf(pf, sizeof(pf), "%s/DevToolsActivePort", o->profile_dir);
        unlink(pf);
        bp->owns_profile = false;
    } else {
        const char *base = getenv("TMPDIR");
        if (!base || !*base) base = "/tmp";
        snprintf(bp->profile_dir, sizeof(bp->profile_dir), "%s/rebellion-XXXXXX", base);
        if (!mkdtemp(bp->profile_dir)) return false;
        bp->owns_profile = true;
    }

    const char *ua = o->ua ? o->ua : c->ua;
    const char *al = o->al ? o->al : c->accept_language;

    char ua_arg[1024], al_arg[256], dir_arg[576], lang_arg[128], proxy_arg[1024];
    snprintf(ua_arg, sizeof(ua_arg), "--user-agent=%s", ua);
    char al_first[64];
    size_t k = 0;
    for (const char *p = al; *p && *p != ',' && *p != ';' && k < sizeof(al_first) - 1;
         p++)
        al_first[k++] = *p;
    al_first[k] = '\0';
    snprintf(al_arg, sizeof(al_arg), "--accept-lang=%s", al);
    snprintf(lang_arg, sizeof(lang_arg), "--lang=%s", al_first);
    snprintf(dir_arg, sizeof(dir_arg), "--user-data-dir=%s", bp->profile_dir);
    proxy_arg[0] = '\0';
    if (o->proxy && *o->proxy)
        snprintf(proxy_arg, sizeof(proxy_arg), "--proxy-server=%s", o->proxy);

    const char *argv[40];
    int a = 0;
    argv[a++] = c->chromium_path;
    argv[a++] = "--headless=new";
    argv[a++] = "--no-sandbox";
    argv[a++] = "--disable-gpu";
    argv[a++] = "--disable-dev-shm-usage";
    argv[a++] = "--disable-extensions";
    argv[a++] = "--disable-background-networking";
    argv[a++] = "--disable-component-update";
    argv[a++] = "--disable-sync";
    argv[a++] = "--no-first-run";
    argv[a++] = "--no-default-browser-check";
    argv[a++] = "--hide-scrollbars";
    argv[a++] = "--mute-audio";
    /* Fetch exactly the URL asked for (no automatic http->https upgrade), so
     * both engines agree on what was scraped. */
    argv[a++] = "--disable-features=HttpsUpgrades";
    argv[a++] = "--window-size=1366,900";
    argv[a++] = "--remote-debugging-port=0";
    argv[a++] = "--remote-debugging-address=127.0.0.1";
    argv[a++] = ua_arg;
    argv[a++] = al_arg;
    argv[a++] = lang_arg;
    argv[a++] = dir_arg;
    if (proxy_arg[0]) {
        argv[a++] = proxy_arg;
        /* Keep loopback targets direct (Chromium's default). */
        argv[a++] = "--proxy-bypass-list=localhost;127.0.0.1;[::1]";
    }
    argv[a++] = "about:blank";
    argv[a] = NULL;

    /* Computed before fork: only async-signal-safe calls happen in the child
     * of a (possibly multi-threaded) parent. */
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 65536) maxfd = 65536;

    pid_t pid = fork();
    if (pid < 0) {
        if (bp->owns_profile) rm_rf(bp->profile_dir);
        return false;
    }
    if (pid == 0) {
        /* Child: own process group, silence output, and drop every inherited
         * descriptor (sockets of other requests, the server's listener...). */
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        bool closed = false;
#ifdef SYS_close_range
        closed = syscall(SYS_close_range, 3U, ~0U, 0U) == 0;
#endif
        if (!closed)
            for (long fd = 3; fd < maxfd; fd++) close((int)fd);
        execv(c->chromium_path, (char *const *)argv);
        _exit(127);
    }
    bp->pid = pid;
    return true;
}

/* Read the DevTools port from <profile>/DevToolsActivePort. */
static int read_devtools_port(const browser_proc *bp, long timeout_ms) {
    char path[600];
    snprintf(path, sizeof(path), "%s/DevToolsActivePort", bp->profile_dir);
    long long deadline = rbl_now_ms() + timeout_ms;
    while (rbl_now_ms() < deadline) {
        FILE *f = fopen(path, "re");
        if (f) {
            char line[64] = {0};
            bool got = fgets(line, sizeof(line), f) != NULL;
            fclose(f);
            int port = got ? atoi(line) : 0;
            if (port > 0) return port;
        }
        /* Stop early if the browser died (bad flags, missing libraries...). */
        if (waitpid(bp->pid, NULL, WNOHANG) == bp->pid) return -1;
        rbl_sleep_ms(50);
    }
    return -1;
}

/* Ask the browser to shut down cleanly (flushes cookies to a persistent
 * profile) via the browser-level DevTools endpoint. Best effort. */
static void browser_close_cdp(int port) {
    char *ver = http_local_get(port, "/json/version", 2000);
    if (!ver) return;
    const char *v = rbl_json_find_value(ver, "webSocketDebuggerUrl");
    char *wsurl = (v && *v == '"') ? rbl_json_decode_string(v, NULL) : NULL;
    free(ver);
    if (!wsurl) return;
    int wsport = port;
    char *path = ws_parse(wsurl, &wsport);
    free(wsurl);
    ws_conn w;
    memset(&w, 0, sizeof(w));
    w.fd = tcp_connect_local(wsport, 2000);
    if (w.fd >= 0) {
        sockrd_init(&w.rd, w.fd);
        if (ws_handshake(&w, wsport, path, 2000)) cdp_send(&w, "Browser.close", NULL);
        close(w.fd);
    }
    free(path);
}

static bool wait_exit(pid_t pid, long ms) {
    long long deadline = rbl_now_ms() + ms;
    for (;;) {
        pid_t r = waitpid(pid, NULL, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) return true;
        if (rbl_now_ms() >= deadline) return false;
        rbl_sleep_ms(25);
    }
}

/* Stop the browser. `graceful` gives it time to exit on its own first (after
 * Browser.close), which persistent profiles need to save their cookies. */
static void kill_browser(browser_proc *bp, bool graceful) {
    if (bp->pid > 0) {
        bool done = graceful && wait_exit(bp->pid, 10000);
        if (!done) {
            kill(-bp->pid, SIGTERM); /* the whole process group */
            kill(bp->pid, SIGTERM);
            done = wait_exit(bp->pid, graceful ? 5000 : 1000);
        }
        if (!done) {
            kill(-bp->pid, SIGKILL);
            kill(bp->pid, SIGKILL);
            waitpid(bp->pid, NULL, 0);
        }
        bp->pid = 0;
    }
    /* Only delete throwaway profiles; persistent ones are kept for reuse. */
    if (bp->owns_profile && bp->profile_dir[0]) rm_rf(bp->profile_dir);
}

static int rm_entry(const char *path, const struct stat *sb, int typeflag,
                    struct FTW *ftw) {
    (void)sb;
    (void)typeflag;
    (void)ftw;
    remove(path); /* unlink files, rmdir dirs (FTW_DEPTH gives children first) */
    return 0;
}

/* Recursively remove a directory tree we created. Best-effort. */
static void rm_rf(const char *path) {
    if (!path || !*path) return;
    if (strcmp(path, "/") == 0 || strstr(path, "..")) return; /* backstop */
    nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
}

/* ---- find a page target WS url -------------------------------------- */

static char *find_page_ws(int port, long timeout_ms) {
    long long deadline = rbl_now_ms() + timeout_ms;
    while (rbl_now_ms() < deadline) {
        char *list = http_local_get(port, "/json/list", 3000);
        if (list) {
            /* Take the debugger URL of the first "page" target. */
            const char *page = strstr(list, "\"type\": \"page\"");
            if (!page) page = strstr(list, "\"type\":\"page\"");
            char *url = NULL;
            if (page) {
                /* Each target is an object; search back to its start. */
                const char *obj = page;
                while (obj > list && *obj != '{') obj--;
                const char *v = rbl_json_find_value(obj, "webSocketDebuggerUrl");
                if (v && *v == '"') url = rbl_json_decode_string(v, NULL);
            }
            free(list);
            if (url) return url;
        }
        rbl_sleep_ms(80);
    }
    return NULL;
}

/* ---- session -------------------------------------------------------- */

struct rbl_session {
    rbl_client *client;
    browser_proc bp;
    ws_conn ws;
    int port;
    long timeout_ms;
    char *current_url;
    int status_code;
    bool open;
    bool holds_slot;
    bool persistent;
    bool respect_robots;
};

static rbl_status session_connect(rbl_session *s, bool intercept_all) {
    s->port = read_devtools_port(&s->bp, 15000);
    if (s->port <= 0) {
        rbl_logf("browser: no DevTools port (did Chromium start?)");
        return RBL_ERR_RENDER;
    }
    rbl_logf("browser: devtools on port %d", s->port);

    char *wsurl = find_page_ws(s->port, 8000);
    if (!wsurl) {
        rbl_logf("browser: no page target");
        return RBL_ERR_RENDER;
    }
    int wsport = s->port;
    char *path = ws_parse(wsurl, &wsport);
    free(wsurl);

    s->ws.fd = tcp_connect_local(wsport, 5000);
    if (s->ws.fd < 0) {
        free(path);
        return RBL_ERR_RENDER;
    }
    sockrd_init(&s->ws.rd, s->ws.fd);
    bool hs = ws_handshake(&s->ws, wsport, path, 5000);
    free(path);
    if (!hs) {
        rbl_logf("browser: ws handshake failed");
        return RBL_ERR_RENDER;
    }
    if (!cdp_call_ok(&s->ws, "Page.enable", NULL, 5000) ||
        !cdp_call_ok(&s->ws, "Runtime.enable", NULL, 5000))
        return RBL_ERR_RENDER;

    /* Main frame id, to tell main-document navigations from iframes. */
    char *tree = cdp_call(&s->ws, "Page.getFrameTree", NULL, 5000);
    if (tree) {
        s->ws.ic.main_frame = json_str(tree, "\"frame\":{", "id");
        free(tree);
    }

    /* Intercept requests when we must answer proxy auth (all requests) or
     * enforce the SSRF guard on navigations (documents only). */
    if (intercept_all || !s->ws.ic.allow_private) {
        const char *params =
            intercept_all
                ? "{\"handleAuthRequests\":true,\"patterns\":[{\"urlPattern\":\"*\"}]}"
                : "{\"patterns\":[{\"urlPattern\":\"*\",\"resourceType\":\"Document\"}]}";
        if (!cdp_call_ok(&s->ws, "Fetch.enable", params, 5000)) return RBL_ERR_RENDER;
        s->ws.ic.enabled = true;
    }
    return RBL_OK;
}

void rbl_session_opts_init(rbl_session_opts *o) {
    memset(o, 0, sizeof(*o));
    o->timeout_ms = 45000;
}

rbl_session *rbl_session_open(rbl_client *c, const rbl_session_opts *in,
                              rbl_status *out) {
    if (!c || !rbl_browser_available(c)) {
        if (out) *out = RBL_ERR_UNSUPPORTED;
        return NULL;
    }
    rbl_session_opts o;
    if (in)
        o = *in;
    else
        rbl_session_opts_init(&o);
    if ((o.user_agent && strpbrk(o.user_agent, "\r\n")) ||
        (o.accept_language && strpbrk(o.accept_language, "\r\n"))) {
        if (out) *out = RBL_ERR_INVALID_ARG;
        return NULL;
    }

    rbl_session *s = (rbl_session *)calloc(1, sizeof(*s));
    if (!s) {
        if (out) *out = RBL_ERR_OOM;
        return NULL;
    }
    s->client = c;
    s->ws.fd = -1;
    s->bp.pid = -1;
    s->timeout_ms = o.timeout_ms > 0 ? o.timeout_ms : 45000;
    s->respect_robots = o.respect_robots;

    const char *profile = o.profile_dir ? o.profile_dir : c->cfg.browser_profile_dir;
    s->persistent = profile && *profile;

    /* Bound live browsers (RAM) and serialize the persistent profile. */
    rbl_status st = rbl_client_browser_acquire(c, s->persistent, s->timeout_ms);
    if (st != RBL_OK) {
        free(s);
        if (out) *out = st;
        return NULL;
    }
    s->holds_slot = true;

    /* Effective proxy: per-session, else the client's pool, else the
     * environment. Credentials are split off for Fetch.authRequired. */
    char *picked = o.proxy ? NULL : rbl_client_pick_proxy(c, NULL);
    const char *raw = o.proxy ? o.proxy : picked;
    if (!raw || !*raw) raw = getenv("HTTPS_PROXY");
    if (!raw || !*raw) raw = getenv("https_proxy");
    char *chrome_proxy = NULL;
    if (raw && *raw &&
        !split_proxy(raw, &chrome_proxy, &s->ws.ic.proxy_user, &s->ws.ic.proxy_pass)) {
        rbl_logf("browser: cannot parse proxy URL");
        free(picked);
        st = RBL_ERR_INVALID_ARG;
        goto fail;
    }
    free(picked);
    s->ws.ic.allow_private = c->cfg.allow_private_networks;
    s->ws.ic.check_dns = chrome_proxy == NULL; /* direct connection */

    launch_opts lo = {o.user_agent, o.accept_language, chrome_proxy, profile};
    bool launched = launch_chromium(c, &lo, &s->bp);
    free(chrome_proxy);
    if (!launched) {
        st = RBL_ERR_RENDER;
        goto fail;
    }
    st = session_connect(s, s->ws.ic.proxy_user != NULL);
    if (st != RBL_OK) goto fail;

    s->open = true;
    if (out) *out = RBL_OK;
    return s;

fail:
    rbl_session_close(s);
    if (out) *out = st;
    return NULL;
}

void rbl_session_close(rbl_session *s) {
    if (!s) return;
    bool graceful = s->persistent && s->port > 0 && s->bp.pid > 0;
    if (graceful) browser_close_cdp(s->port);
    if (s->ws.fd >= 0) close(s->ws.fd);
    kill_browser(&s->bp, graceful);
    if (s->holds_slot) rbl_client_browser_release(s->client, s->persistent);
    free(s->ws.ic.proxy_user);
    free(s->ws.ic.proxy_pass);
    free(s->ws.ic.main_frame);
    free(s->current_url);
    free(s);
}

const char *rbl_session_url(rbl_session *s) {
    return s ? s->current_url : NULL;
}

rbl_status rbl_session_last_error(rbl_session *s) {
    if (!s) return RBL_ERR_INVALID_ARG;
    return s->ws.ic.blocked_main ? RBL_ERR_BLOCKED : RBL_OK;
}

int rbl_session_status_code(rbl_session *s) {
    return s ? s->status_code : 0;
}

/* Refresh current URL and main-document HTTP status after a navigation. */
static void session_refresh(rbl_session *s) {
    char *cur = cdp_eval_string(&s->ws, "location.href", 5000);
    if (cur) {
        free(s->current_url);
        s->current_url = cur;
    }
    char *code = cdp_eval_string(
        &s->ws,
        "String((performance.getEntriesByType('navigation')[0]||{}).responseStatus||0)",
        5000);
    s->status_code = code ? atoi(code) : 0;
    free(code);
}

rbl_status rbl_session_goto(rbl_session *s, const char *url, long settle_ms) {
    if (!s || !s->open || !url) return RBL_ERR_INVALID_ARG;

    /* http/https only: never file://, chrome://, javascript: ... */
    rbl_url u;
    if (!rbl_url_parse(url, &u)) return RBL_ERR_BAD_URL;
    rbl_status st = RBL_OK;
    if (!s->client->cfg.allow_private_networks &&
        (s->ws.ic.check_dns ? rbl_host_resolves_private(u.host)
                            : rbl_host_is_private(u.host))) {
        rbl_logf("SSRF guard: refusing private host in %s", url);
        st = RBL_ERR_BLOCKED;
    } else if (s->respect_robots && !rbl_robots_allowed(s->client, &u, "rebellion")) {
        st = RBL_ERR_ROBOTS_DENIED;
    }
    rbl_url_free(&u);
    if (st != RBL_OK) return st;

    s->ws.ic.blocked_main = false;
    s->ws.ic.auth_attempts = 0;
    long long nav_deadline = rbl_now_ms() + s->timeout_ms;
    st = cdp_navigate(&s->ws, url, s->timeout_ms);
    if (st == RBL_OK && s->ws.ic.blocked_main) st = RBL_ERR_BLOCKED;
    if (st != RBL_OK) return st;

    /* Wait for the document to finish loading, servicing events meanwhile. */
    for (;;) {
        long remain = (long)(nav_deadline - rbl_now_ms());
        if (remain <= 0) break;
        char *rs =
            cdp_eval_string(&s->ws, "document.readyState", remain < 3000 ? remain : 3000);
        bool complete = rs && strcmp(rs, "complete") == 0;
        free(rs);
        if (complete) break;
        cdp_pump(&s->ws, 150);
    }
    cdp_pump(&s->ws, settle_ms >= 0 ? settle_ms : 500);
    if (s->ws.ic.blocked_main) return RBL_ERR_BLOCKED; /* e.g. JS redirect */
    session_refresh(s);
    return RBL_OK;
}

/* Run a JS snippet expected to return 'OK' / 'NOTFOUND'. */
static rbl_status run_action(rbl_session *s, rbl_buf *js) {
    char *r = cdp_eval_string(&s->ws, rbl_buf_cstr(js), s->timeout_ms);
    rbl_buf_free(js);
    if (!r) return RBL_ERR_RENDER;
    rbl_status st = strcmp(r, "OK") == 0
                        ? RBL_OK
                        : (strcmp(r, "NOTFOUND") == 0 ? RBL_ERR_PARSE : RBL_ERR_RENDER);
    free(r);
    /* Give any navigation the action started a moment to begin, then
     * service it (requests stay paused only while nobody is listening). */
    cdp_pump(&s->ws, 100);
    session_refresh(s);
    return st;
}

static void js_query(rbl_buf *js, const char *selector) {
    rbl_buf_init(js);
    rbl_buf_append_str(js, "(function(){var e=document.querySelector(");
    rbl_json_quote(js, selector);
    rbl_buf_append_str(js, ");if(!e)return 'NOTFOUND';");
}

rbl_status rbl_session_click(rbl_session *s, const char *selector) {
    if (!s || !s->open || !selector) return RBL_ERR_INVALID_ARG;
    rbl_buf js;
    js_query(&js, selector);
    rbl_buf_append_str(&js, "e.scrollIntoView({block:'center'});e.click();"
                            "return 'OK';})()");
    return run_action(s, &js);
}

rbl_status rbl_session_fill(rbl_session *s, const char *selector, const char *value) {
    if (!s || !s->open || !selector) return RBL_ERR_INVALID_ARG;
    rbl_buf js;
    js_query(&js, selector);
    rbl_buf_append_str(&js, "e.focus();var v=");
    rbl_json_quote(&js, value ? value : "");
    rbl_buf_append_str(&js, ";if('value' in e){e.value=v;}else{e.textContent=v;}"
                            "e.dispatchEvent(new Event('input',{bubbles:true}));"
                            "e.dispatchEvent(new Event('change',{bubbles:true}));"
                            "return 'OK';})()");
    return run_action(s, &js);
}

rbl_status rbl_session_submit(rbl_session *s, const char *selector) {
    if (!s || !s->open || !selector) return RBL_ERR_INVALID_ARG;
    rbl_buf js;
    js_query(&js, selector);
    rbl_buf_append_str(&js,
                       "if(e.tagName==='FORM'){e.requestSubmit?e.requestSubmit():e.submit();}"
                       "else if(e.form){e.form.requestSubmit?e.form.requestSubmit(e):"
                       "e.form.submit();}else{e.click();}return 'OK';})()");
    return run_action(s, &js);
}

rbl_status rbl_session_scroll(rbl_session *s, int dx, int dy) {
    if (!s || !s->open) return RBL_ERR_INVALID_ARG;
    rbl_buf js;
    rbl_buf_init(&js);
    rbl_buf_appendf(&js, "(function(){window.scrollBy(%d,%d);return 'OK';})()", dx, dy);
    return run_action(s, &js);
}

rbl_status rbl_session_scroll_to_bottom(rbl_session *s) {
    if (!s || !s->open) return RBL_ERR_INVALID_ARG;
    rbl_buf js;
    rbl_buf_init(&js);
    rbl_buf_append_str(&js, "(function(){window.scrollTo(0,Math.max("
                            "document.body?document.body.scrollHeight:0,"
                            "document.documentElement.scrollHeight));"
                            "return 'OK';})()");
    return run_action(s, &js);
}

rbl_status rbl_session_wait_for(rbl_session *s, const char *selector, long timeout_ms) {
    if (!s || !s->open || !selector) return RBL_ERR_INVALID_ARG;
    long long deadline = rbl_now_ms() + (timeout_ms > 0 ? timeout_ms : s->timeout_ms);
    for (;;) {
        rbl_buf js;
        rbl_buf_init(&js);
        rbl_buf_append_str(&js, "(function(){return document.querySelector(");
        rbl_json_quote(&js, selector);
        rbl_buf_append_str(&js, ")?'yes':'no';})()");
        char *r = cdp_eval_string(&s->ws, rbl_buf_cstr(&js), 2000);
        rbl_buf_free(&js);
        bool found = r && strcmp(r, "yes") == 0;
        free(r);
        if (found) return RBL_OK;
        if (rbl_now_ms() >= deadline) return RBL_ERR_TIMEOUT;
        cdp_pump(&s->ws, 150);
    }
}

rbl_status rbl_session_wait_ms(rbl_session *s, long ms) {
    if (!s || !s->open) return RBL_ERR_INVALID_ARG;
    if (ms > 0) cdp_pump(&s->ws, ms);
    session_refresh(s);
    return RBL_OK;
}

rbl_status rbl_session_eval(rbl_session *s, const char *js, char **out_result) {
    if (out_result) *out_result = NULL;
    if (!s || !s->open || !js) return RBL_ERR_INVALID_ARG;
    /* Coerce the result to a string so numbers/booleans come back too. */
    rbl_buf expr;
    rbl_buf_init(&expr);
    rbl_buf_append_str(&expr, "String(");
    rbl_buf_append_str(&expr, js);
    rbl_buf_append_str(&expr, ")");
    char *r = cdp_eval_string(&s->ws, rbl_buf_cstr(&expr), s->timeout_ms);
    rbl_buf_free(&expr);
    if (!r) return RBL_ERR_RENDER;
    if (out_result)
        *out_result = r;
    else
        free(r);
    return RBL_OK;
}

char *rbl_session_html(rbl_session *s) {
    if (!s || !s->open) return NULL;
    /* The current document is an error page for a blocked navigation. */
    if (s->ws.ic.blocked_main) return NULL;
    return cdp_eval_string(
        &s->ws, "document.documentElement ? document.documentElement.outerHTML : ''",
        15000);
}

unsigned char *rbl_session_screenshot(rbl_session *s, bool full_page, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!s || !s->open || s->ws.ic.blocked_main) return NULL;
    size_t pl = 0;
    unsigned char *png = cdp_screenshot(&s->ws, full_page, 20000, &pl);
    if (out_len) *out_len = pl;
    return png;
}

/* ---- one-shot render (built on a session) --------------------------- */

rbl_status rbl_browser_render(rbl_client *c, const char *url,
                              const rbl_render_opts *opts, char **out_html,
                              char **out_final_url, int *out_status_code,
                              unsigned char **out_png, size_t *out_png_len) {
    if (out_html) *out_html = NULL;
    if (out_final_url) *out_final_url = NULL;
    if (out_status_code) *out_status_code = 0;
    if (out_png) *out_png = NULL;
    if (out_png_len) *out_png_len = 0;
    if (!rbl_browser_available(c)) return RBL_ERR_UNSUPPORTED;

    rbl_session_opts so;
    rbl_session_opts_init(&so);
    so.user_agent = opts->user_agent;
    so.accept_language = opts->accept_language;
    so.timeout_ms = opts->timeout_ms > 0 ? opts->timeout_ms : 45000;

    rbl_status st = RBL_ERR_RENDER;
    rbl_session *s = rbl_session_open(c, &so, &st);
    if (!s) return st;

    st = rbl_session_goto(s, url, opts->settle_ms >= 0 ? opts->settle_ms : 800);
    if (st != RBL_OK) {
        rbl_session_close(s);
        return st;
    }
    char *html = rbl_session_html(s);
    if (!html) {
        st = rbl_session_last_error(s);
        rbl_session_close(s);
        return st != RBL_OK ? st : RBL_ERR_RENDER;
    }
    if (out_html)
        *out_html = html;
    else
        free(html);
    if (out_final_url) {
        const char *cur = rbl_session_url(s);
        *out_final_url = rbl_strdup(cur ? cur : url);
    }
    if (out_status_code) *out_status_code = rbl_session_status_code(s);
    if (opts->screenshot && out_png) {
        size_t pl = 0;
        unsigned char *png = rbl_session_screenshot(s, opts->full_page, &pl);
        if (png) {
            *out_png = png;
            if (out_png_len) *out_png_len = pl;
        }
    }
    rbl_session_close(s);
    return RBL_OK;
}
