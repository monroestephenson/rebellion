#include "browser.h"
#include "internal.h"
#include "json.h"
#include "net.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
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

static int tcp_connect_local(int port, long timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    long long deadline = rbl_now_ms() + timeout_ms;
    for (;;) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            return fd;
        }
        if (rbl_now_ms() > deadline) {
            close(fd);
            return -1;
        }
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
    for (;;) {
        int f = sockrd_fill(&r, timeout_ms);
        if (f <= 0) break;
        rbl_buf_append(&resp, r.buf + r.start, r.end - r.start);
        r.start = r.end;
    }
    close(fd);

    char *s = rbl_buf_cstr(&resp);
    char *body = s ? strstr(s, "\r\n\r\n") : NULL;
    char *out = body ? rbl_strdup(body + 4) : NULL;
    rbl_buf_free(&resp);
    return out;
}

/* ---- WebSocket (client) --------------------------------------------- */

typedef struct {
    int fd;
    sockrd rd;
    int next_id;
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
    /* A random 16-byte key, base64 of fixed sample is fine for localhost. */
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

    /* Read status line + headers up to blank line. */
    rbl_buf line;
    rbl_buf_init(&line);
    int nl_state = 0; /* count of consecutive CRLF */
    bool is101 = false;
    rbl_buf statusline;
    rbl_buf_init(&statusline);
    bool first_line = true;
    for (;;) {
        int ch = sockrd_getc(&w->rd, timeout_ms);
        if (ch < 0) {
            rbl_buf_free(&line);
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
            nl_state++;
            if (nl_state == 2) break; /* blank line => end of headers */
            continue;
        }
        nl_state = 0;
        if (first_line) rbl_buf_append_ch(&statusline, (char)ch);
    }
    rbl_buf_free(&line);
    rbl_buf_free(&statusline);
    return is101;
}

static bool ws_send_text(ws_conn *w, const char *data, size_t len) {
    unsigned char hdr[14];
    size_t hn = 0;
    hdr[hn++] = 0x81; /* FIN + text */
    unsigned char mask_bit = 0x80;
    if (len < 126) {
        hdr[hn++] = mask_bit | (unsigned char)len;
    } else if (len <= 0xFFFF) {
        hdr[hn++] = mask_bit | 126;
        hdr[hn++] = (unsigned char)(len >> 8);
        hdr[hn++] = (unsigned char)(len & 0xFF);
    } else {
        hdr[hn++] = mask_bit | 127;
        for (int i = 7; i >= 0; i--)
            hdr[hn++] = (unsigned char)((len >> (i * 8)) & 0xFF);
    }
    unsigned char mk[4];
    ws_mask_key(mk);
    memcpy(hdr + hn, mk, 4);
    hn += 4;
    if (!send_all(w->fd, hdr, hn)) return false;

    /* Mask and send payload in chunks. */
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
        if (remain <= 0) {
            rbl_buf_free(&msg);
            return NULL;
        }

        unsigned char h2[2];
        if (!sockrd_read_n(&w->rd, h2, 2, remain)) {
            rbl_buf_free(&msg);
            return NULL;
        }
        int fin = (h2[0] & 0x80) != 0;
        int opcode = h2[0] & 0x0F;
        int masked = (h2[1] & 0x80) != 0;
        uint64_t plen = h2[1] & 0x7F;

        if (plen == 126) {
            unsigned char e[2];
            if (!sockrd_read_n(&w->rd, e, 2, remain)) {
                rbl_buf_free(&msg);
                return NULL;
            }
            plen = ((uint64_t)e[0] << 8) | e[1];
        } else if (plen == 127) {
            unsigned char e[8];
            if (!sockrd_read_n(&w->rd, e, 8, remain)) {
                rbl_buf_free(&msg);
                return NULL;
            }
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | e[i];
        }
        unsigned char mk[4] = {0, 0, 0, 0};
        if (masked && !sockrd_read_n(&w->rd, mk, 4, remain)) {
            rbl_buf_free(&msg);
            return NULL;
        }

        /* Read payload. Cap to avoid runaway (256 MiB). */
        if (plen > (uint64_t)256 * 1024 * 1024) {
            rbl_buf_free(&msg);
            return NULL;
        }
        unsigned char *payload = (unsigned char *)malloc((size_t)plen + 1);
        if (!payload) {
            rbl_buf_free(&msg);
            return NULL;
        }
        if (plen && !sockrd_read_n(&w->rd, payload, (size_t)plen, remain)) {
            free(payload);
            rbl_buf_free(&msg);
            return NULL;
        }
        if (masked)
            for (uint64_t i = 0; i < plen; i++) payload[i] ^= mk[i & 3];

        if (opcode == 0x8) { /* close */
            free(payload);
            rbl_buf_free(&msg);
            return NULL;
        } else if (opcode == 0x9) { /* ping -> pong */
            unsigned char ph[2] = {0x8A,
                                   (unsigned char)(0x80 | (plen < 126 ? plen : 0))};
            /* reply with empty pong to keep it simple */
            unsigned char pong[6] = {0x8A, 0x80, 0, 0, 0, 0};
            (void)ph;
            send_all(w->fd, pong, sizeof(pong));
            free(payload);
            continue;
        } else if (opcode == 0xA) { /* pong */
            free(payload);
            continue;
        }
        /* text (1) or continuation (0) */
        rbl_buf_append(&msg, payload, (size_t)plen);
        free(payload);
        if (fin) break;
    }
    return rbl_buf_steal(&msg);
}

/* ---- CDP ------------------------------------------------------------ */

/* CDP message ids are per-connection (see ws_conn.next_id). */

/* Send a Runtime.evaluate and return the decoded string value, or NULL. */
static char *cdp_eval_string(ws_conn *w, const char *expr, long timeout_ms) {
    int id = ++w->next_id;
    rbl_buf cmd;
    rbl_buf_init(&cmd);
    rbl_buf_appendf(&cmd,
                    "{\"id\":%d,\"method\":\"Runtime.evaluate\",\"params\":{"
                    "\"expression\":",
                    id);
    rbl_json_quote(&cmd, expr);
    rbl_buf_append_str(&cmd, ",\"returnByValue\":true,\"awaitPromise\":true}}");
    bool ok = ws_send_text(w, cmd.data, cmd.len);
    rbl_buf_free(&cmd);
    if (!ok) return NULL;

    char want[32];
    snprintf(want, sizeof(want), "\"id\":%d", id);
    long long deadline = rbl_now_ms() + timeout_ms;
    for (;;) {
        long remain = (long)(deadline - rbl_now_ms());
        if (remain <= 0) return NULL;
        char *msg = ws_recv_text(w, remain);
        if (!msg) return NULL;
        if (strstr(msg, want)) {
            /* Locate "result":{"result":{..."value":<str>}} */
            const char *res = strstr(msg, "\"result\"");
            const char *v = res ? rbl_json_find_value(res, "value") : NULL;
            char *out = NULL;
            if (v && *v == '"') out = rbl_json_decode_string(v, NULL);
            free(msg);
            return out;
        }
        free(msg);
    }
}

/* Send a bare method (no params needed) and wait for its id response. */
static bool cdp_call_simple(ws_conn *w, const char *method, long timeout_ms) {
    int id = ++w->next_id;
    rbl_buf cmd;
    rbl_buf_init(&cmd);
    rbl_buf_appendf(&cmd, "{\"id\":%d,\"method\":\"%s\"}", id, method);
    bool ok = ws_send_text(w, cmd.data, cmd.len);
    rbl_buf_free(&cmd);
    if (!ok) return false;
    char want[32];
    snprintf(want, sizeof(want), "\"id\":%d", id);
    long long deadline = rbl_now_ms() + timeout_ms;
    for (;;) {
        long remain = (long)(deadline - rbl_now_ms());
        if (remain <= 0) return false;
        char *msg = ws_recv_text(w, remain);
        if (!msg) return false;
        bool hit = strstr(msg, want) != NULL;
        free(msg);
        if (hit) return true;
    }
}

static bool cdp_navigate(ws_conn *w, const char *url, long timeout_ms) {
    int id = ++w->next_id;
    rbl_buf cmd;
    rbl_buf_init(&cmd);
    rbl_buf_appendf(&cmd,
                    "{\"id\":%d,\"method\":\"Page.navigate\",\"params\":{\"url\":", id);
    rbl_json_quote(&cmd, url);
    rbl_buf_append_str(&cmd, "}}");
    bool ok = ws_send_text(w, cmd.data, cmd.len);
    rbl_buf_free(&cmd);
    if (!ok) return false;
    char want[32];
    snprintf(want, sizeof(want), "\"id\":%d", id);
    long long deadline = rbl_now_ms() + timeout_ms;
    for (;;) {
        long remain = (long)(deadline - rbl_now_ms());
        if (remain <= 0) return false;
        char *msg = ws_recv_text(w, remain);
        if (!msg) return false;
        bool hit = strstr(msg, want) != NULL;
        /* An error navigating still returns an id response. */
        free(msg);
        if (hit) return true;
    }
}

static char *cdp_screenshot(ws_conn *w, bool full_page, long timeout_ms,
                            size_t *out_len) {
    int id = ++w->next_id;
    rbl_buf cmd;
    rbl_buf_init(&cmd);
    rbl_buf_appendf(
        &cmd,
        "{\"id\":%d,\"method\":\"Page.captureScreenshot\",\"params\":{"
        "\"format\":\"png\",\"captureBeyondViewport\":%s,\"fromSurface\":true}}",
        id, full_page ? "true" : "false");
    bool ok = ws_send_text(w, cmd.data, cmd.len);
    rbl_buf_free(&cmd);
    if (!ok) return NULL;
    char want[32];
    snprintf(want, sizeof(want), "\"id\":%d", id);
    long long deadline = rbl_now_ms() + timeout_ms;
    for (;;) {
        long remain = (long)(deadline - rbl_now_ms());
        if (remain <= 0) return NULL;
        char *msg = ws_recv_text(w, remain);
        if (!msg) return NULL;
        if (strstr(msg, want)) {
            const char *v = rbl_json_find_value(msg, "data");
            char *b64 = (v && *v == '"') ? rbl_json_decode_string(v, NULL) : NULL;
            free(msg);
            if (!b64) return NULL;
            unsigned char *png = rbl_base64_decode(b64, strlen(b64), out_len);
            free(b64);
            return (char *)png;
        }
        free(msg);
    }
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
    const char *proxy;       /* NULL => fall back to env proxy          */
    const char *profile_dir; /* NULL => throwaway temp profile          */
} launch_opts;

static void rm_rf(const char *path); /* fwd */

static bool launch_chromium(rbl_client *c, const launch_opts *o, browser_proc *bp) {
    if (o->profile_dir && *o->profile_dir) {
        /* Persistent profile: create if needed, keep it on close. */
        mkdir(o->profile_dir, 0700); /* ok if it already exists */
        snprintf(bp->profile_dir, sizeof(bp->profile_dir), "%s", o->profile_dir);
        bp->owns_profile = false;
    } else {
        const char *base = getenv("TMPDIR");
        if (!base || !*base) base = "/tmp";
        snprintf(bp->profile_dir, sizeof(bp->profile_dir), "%s/rebellion-XXXXXX", base);
        if (!mkdtemp(bp->profile_dir))
            snprintf(bp->profile_dir, sizeof(bp->profile_dir), "%s/rebellion-%d", base,
                     (int)getpid());
        bp->owns_profile = true;
    }

    const char *ua = o->ua ? o->ua : c->ua;
    const char *al = o->al ? o->al : c->accept_language;

    char ua_arg[1024], al_arg[256], dir_arg[576], lang_arg[128];
    snprintf(ua_arg, sizeof(ua_arg), "--user-agent=%s", ua);
    /* Accept-Language header: first token of the list. */
    char al_first[64];
    size_t k = 0;
    for (const char *p = al; *p && *p != ',' && k < sizeof(al_first) - 1; p++)
        al_first[k++] = *p;
    al_first[k] = '\0';
    snprintf(al_arg, sizeof(al_arg), "--accept-lang=%s", al);
    snprintf(lang_arg, sizeof(lang_arg), "--lang=%s", al_first);
    snprintf(dir_arg, sizeof(dir_arg), "--user-data-dir=%s", bp->profile_dir);

    const char *proxy = o->proxy;
    if (!proxy || !*proxy) proxy = getenv("HTTPS_PROXY");
    if (!proxy || !*proxy) proxy = getenv("https_proxy");
    char proxy_arg[1024] = {0};
    if (proxy && *proxy)
        snprintf(proxy_arg, sizeof(proxy_arg), "--proxy-server=%s", proxy);

    const char *argv[32];
    int a = 0;
    argv[a++] = c->chromium_path;
    argv[a++] = "--headless=new";
    argv[a++] = "--no-sandbox";
    argv[a++] = "--disable-gpu";
    argv[a++] = "--disable-dev-shm-usage";
    argv[a++] = "--disable-extensions";
    argv[a++] = "--disable-background-networking";
    argv[a++] = "--no-first-run";
    argv[a++] = "--no-default-browser-check";
    argv[a++] = "--hide-scrollbars";
    argv[a++] = "--mute-audio";
    argv[a++] = "--window-size=1366,900";
    argv[a++] = "--remote-debugging-port=0";
    argv[a++] = "--remote-debugging-address=127.0.0.1";
    argv[a++] = ua_arg;
    argv[a++] = al_arg;
    argv[a++] = lang_arg;
    argv[a++] = dir_arg;
    if (proxy_arg[0]) {
        argv[a++] = proxy_arg;
        /* Keep loopback/localhost direct (Chromium's default), so a local
         * target isn't forced through the proxy. */
        argv[a++] = "--proxy-bypass-list=localhost;127.0.0.1;[::1]";
    }
    argv[a++] = "about:blank";
    argv[a] = NULL;

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        /* Child: silence output, own process group so we can kill cleanly. */
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execv(c->chromium_path, (char *const *)argv);
        _exit(127);
    }
    bp->pid = pid;
    return true;
}

/* Read the DevTools port from <profile>/DevToolsActivePort. */
static int read_devtools_port(const char *profile_dir, long timeout_ms) {
    char path[600];
    snprintf(path, sizeof(path), "%s/DevToolsActivePort", profile_dir);
    long long deadline = rbl_now_ms() + timeout_ms;
    while (rbl_now_ms() < deadline) {
        FILE *f = fopen(path, "r");
        if (f) {
            char line[64] = {0};
            if (fgets(line, sizeof(line), f)) {
                int port = atoi(line);
                fclose(f);
                if (port > 0) return port;
            } else
                fclose(f);
        }
        rbl_sleep_ms(50);
    }
    return -1;
}

static void kill_browser(browser_proc *bp) {
    if (bp->pid > 0) {
        kill(-bp->pid, SIGTERM); /* kill the process group */
        kill(bp->pid, SIGTERM);
        for (int i = 0; i < 20; i++) {
            int st;
            if (waitpid(bp->pid, &st, WNOHANG) == bp->pid) {
                bp->pid = 0;
                break;
            }
            rbl_sleep_ms(25);
        }
        if (bp->pid > 0) {
            kill(-bp->pid, SIGKILL);
            waitpid(bp->pid, NULL, 0);
        }
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
    /* Refuse obviously unsafe targets as a backstop. */
    if (strcmp(path, "/") == 0 || strstr(path, "..")) return;
    nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
}

/* ---- find a page target WS url -------------------------------------- */

static char *find_page_ws(int port, long timeout_ms) {
    long long deadline = rbl_now_ms() + timeout_ms;
    while (rbl_now_ms() < deadline) {
        char *list = http_local_get(port, "/json/list", 3000);
        if (!list) list = http_local_get(port, "/json", 3000);
        if (list) {
            /* Prefer a target of type "page"; else take the first debugger
             * URL present. */
            const char *scan = list;
            const char *page = strstr(list, "\"type\": \"page\"");
            if (!page) page = strstr(list, "\"type\":\"page\"");
            /* Search for the websocket URL near the page entry, else anywhere. */
            const char *v =
                rbl_json_find_value(page ? list : scan, "webSocketDebuggerUrl");
            char *url = (v && *v == '"') ? rbl_json_decode_string(v, NULL) : NULL;
            free(list);
            if (url) return url;
        }
        rbl_sleep_ms(80);
    }
    return NULL;
}

/* ---- session ------------------------------------------------------- */

struct rbl_session {
    rbl_client *client;
    browser_proc bp;
    ws_conn ws;
    long timeout_ms;
    char *current_url;
    bool open;
};

static rbl_status session_connect(rbl_session *s) {
    int port = read_devtools_port(s->bp.profile_dir, 12000);
    if (port <= 0) {
        rbl_logf("browser: no devtools port");
        return RBL_ERR_RENDER;
    }
    rbl_logf("browser: devtools on port %d", port);

    char *wsurl = find_page_ws(port, 8000);
    if (!wsurl) {
        rbl_logf("browser: no page target");
        return RBL_ERR_RENDER;
    }
    int wsport = port;
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
    cdp_call_simple(&s->ws, "Page.enable", 5000);
    cdp_call_simple(&s->ws, "Runtime.enable", 5000);
    return RBL_OK;
}

void rbl_session_opts_init(rbl_session_opts *o) {
    memset(o, 0, sizeof(*o));
    o->timeout_ms = 45000;
}

rbl_session *rbl_session_open(rbl_client *c, const rbl_session_opts *in,
                              rbl_status *out) {
    rbl_status st = RBL_ERR_RENDER;
    if (!c || !rbl_browser_available(c)) {
        if (out) *out = RBL_ERR_UNSUPPORTED;
        return NULL;
    }
    rbl_session_opts o;
    if (in)
        o = *in;
    else
        rbl_session_opts_init(&o);

    rbl_session *s = (rbl_session *)calloc(1, sizeof(*s));
    if (!s) {
        if (out) *out = RBL_ERR_OOM;
        return NULL;
    }
    s->client = c;
    s->ws.fd = -1;
    s->bp.pid = -1;
    s->timeout_ms = o.timeout_ms > 0 ? o.timeout_ms : 45000;

    char *proxy = o.proxy ? rbl_strdup(o.proxy) : rbl_client_pick_proxy(c, NULL);
    const char *profile = o.profile_dir ? o.profile_dir : c->cfg.browser_profile_dir;
    launch_opts lo = {o.user_agent, o.accept_language, proxy, profile};
    bool launched = launch_chromium(c, &lo, &s->bp);
    free(proxy);
    if (!launched) {
        st = RBL_ERR_RENDER;
        goto fail;
    }
    st = session_connect(s);
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
    if (s->ws.fd >= 0) close(s->ws.fd);
    kill_browser(&s->bp);
    free(s->current_url);
    free(s);
}

const char *rbl_session_url(rbl_session *s) {
    return s ? s->current_url : NULL;
}

rbl_status rbl_session_goto(rbl_session *s, const char *url, long settle_ms) {
    if (!s || !s->open || !url) return RBL_ERR_INVALID_ARG;
    if (!s->client->cfg.allow_private_networks && rbl_url_host_is_private(url)) {
        rbl_logf("SSRF guard: refusing private host in %s", url);
        return RBL_ERR_BLOCKED;
    }
    long budget = s->timeout_ms;
    long settle = settle_ms >= 0 ? settle_ms : 500;
    long long nav_deadline = rbl_now_ms() + budget;
    if (!cdp_navigate(&s->ws, url, budget)) return RBL_ERR_RENDER;

    for (;;) {
        long remain = (long)(nav_deadline - rbl_now_ms());
        if (remain <= 0) break;
        char *rs = cdp_eval_string(&s->ws, "document.readyState",
                                   remain < 3000 ? remain : 3000);
        bool complete = rs && strcmp(rs, "complete") == 0;
        free(rs);
        if (complete) break;
        rbl_sleep_ms(150);
    }
    rbl_sleep_ms(settle);

    char *cur = cdp_eval_string(&s->ws, "location.href", 5000);
    if (cur) {
        free(s->current_url);
        s->current_url = cur;
    }
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
    return st;
}

rbl_status rbl_session_click(rbl_session *s, const char *selector) {
    if (!s || !s->open || !selector) return RBL_ERR_INVALID_ARG;
    rbl_buf js;
    rbl_buf_init(&js);
    rbl_buf_append_str(&js, "(function(){var e=document.querySelector(");
    rbl_json_quote(&js, selector);
    rbl_buf_append_str(&js, ");if(!e)return 'NOTFOUND';"
                            "e.scrollIntoView({block:'center'});e.click();"
                            "return 'OK';})()");
    return run_action(s, &js);
}

rbl_status rbl_session_fill(rbl_session *s, const char *selector, const char *value) {
    if (!s || !s->open || !selector) return RBL_ERR_INVALID_ARG;
    rbl_buf js;
    rbl_buf_init(&js);
    rbl_buf_append_str(&js, "(function(){var e=document.querySelector(");
    rbl_json_quote(&js, selector);
    rbl_buf_append_str(&js, ");if(!e)return 'NOTFOUND';e.focus();var v=");
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
    rbl_buf_init(&js);
    rbl_buf_append_str(&js, "(function(){var e=document.querySelector(");
    rbl_json_quote(&js, selector);
    rbl_buf_append_str(
        &js, ");if(!e)return 'NOTFOUND';"
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
        rbl_sleep_ms(150);
    }
}

rbl_status rbl_session_wait_ms(rbl_session *s, long ms) {
    if (!s || !s->open) return RBL_ERR_INVALID_ARG;
    rbl_sleep_ms(ms);
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
    return cdp_eval_string(
        &s->ws, "document.documentElement ? document.documentElement.outerHTML : ''",
        15000);
}

unsigned char *rbl_session_screenshot(rbl_session *s, bool full_page, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!s || !s->open) return NULL;
    size_t pl = 0;
    char *png = cdp_screenshot(&s->ws, full_page, 20000, &pl);
    if (out_len) *out_len = pl;
    return (unsigned char *)png;
}

/* ---- one-shot render (built on a session) --------------------------- */

rbl_status rbl_browser_render(rbl_client *c, const char *url,
                              const rbl_render_opts *opts, char **out_html,
                              char **out_final_url, unsigned char **out_png,
                              size_t *out_png_len) {
    if (out_html) *out_html = NULL;
    if (out_final_url) *out_final_url = NULL;
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
        rbl_session_close(s);
        return RBL_ERR_RENDER;
    }
    if (out_html)
        *out_html = html;
    else
        free(html);
    if (out_final_url) {
        const char *cur = rbl_session_url(s);
        *out_final_url = rbl_strdup(cur ? cur : url);
    }
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
