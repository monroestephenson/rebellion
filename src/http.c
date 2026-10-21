#include "http.h"
#include "internal.h"
#include "charset.h"
#include "net.h"
#include "url.h"
#include "util.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    rbl_buf body;
    char *content_type; /* captured from headers of the current hop */
    size_t max_bytes;
    bool too_large;
} fetch_ctx;

static size_t write_body(char *ptr, size_t size, size_t nmemb, void *ud) {
    fetch_ctx *ctx = (fetch_ctx *)ud;
    size_t n = size * nmemb;
    if (ctx->body.len + n > ctx->max_bytes) {
        ctx->too_large = true;
        return 0; /* aborts the transfer with CURLE_WRITE_ERROR */
    }
    if (!rbl_buf_append(&ctx->body, ptr, n)) return 0;
    return n;
}

static size_t header_cb(char *buffer, size_t size, size_t nitems, void *ud) {
    fetch_ctx *ctx = (fetch_ctx *)ud;
    size_t n = size * nitems;
    if (n > 14 && strncasecmp(buffer, "Content-Type:", 13) == 0) {
        const char *v = buffer + 13;
        size_t vl = n - 13;
        while (vl && (*v == ' ' || *v == '\t')) {
            v++;
            vl--;
        }
        while (vl && (v[vl - 1] == '\r' || v[vl - 1] == '\n' || v[vl - 1] == ' ' ||
                      v[vl - 1] == '\t'))
            vl--;
        free(ctx->content_type);
        ctx->content_type = rbl_strndup(v, vl);
    }
    return n;
}

static bool ct_is_html(const char *ct) {
    if (!ct) return false;
    return strcasestr(ct, "text/html") || strcasestr(ct, "application/xhtml") ||
           strcasestr(ct, "text/xml") || strcasestr(ct, "application/xml") ||
           strcasestr(ct, "application/rss") || strcasestr(ct, "+xml");
}

static bool ct_is_text(const char *ct) {
    if (!ct) return false;
    return ct_is_html(ct) || strncasecmp(ct, "text/", 5) == 0 ||
           strcasestr(ct, "json") || strcasestr(ct, "javascript") ||
           strcasestr(ct, "ecmascript") || strcasestr(ct, "x-www-form-urlencoded");
}

/* No Content-Type: decide from the bytes. Text if the first KiB has no NUL;
 * HTML if it starts (after whitespace/BOM) with a tag-ish prefix. */
static void sniff(const char *data, size_t len, bool *is_text, bool *is_html) {
    size_t n = len < 1024 ? len : 1024;
    *is_text = memchr(data, '\0', n) == NULL;
    *is_html = false;
    if (!*is_text) return;
    size_t i = 0;
    if (len >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF)
        i = 3;
    while (i < n &&
           (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n'))
        i++;
    if (i < n && data[i] == '<') {
        const char *rest = data + i;
        size_t rl = len - i;
        *is_html = (rl >= 9 && strncasecmp(rest, "<!doctype", 9) == 0) ||
                   (rl >= 5 && strncasecmp(rest, "<html", 5) == 0) ||
                   (rl >= 5 && strncasecmp(rest, "<head", 5) == 0) ||
                   (rl >= 5 && strncasecmp(rest, "<body", 5) == 0) ||
                   (rl >= 4 && strncasecmp(rest, "<!--", 4) == 0) ||
                   (rl >= 5 && strncasecmp(rest, "<?xml", 5) == 0);
    }
}

/* Header values must not contain control characters (CR/LF would allow
 * header injection when the value comes from an API caller). */
static bool has_ctl(const char *s) {
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c < 0x20 && c != '\t') || c == 0x7F) return true;
    }
    return false;
}

static bool is_redirect(long code) {
    return code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
}

/* These options are enum values (not macros), so detect them by libcurl
 * version: PREREQFUNCTION since 7.80.0, PROTOCOLS_STR since 7.85.0. */
#define RBL_CURL_HAS_PREREQ (LIBCURL_VERSION_NUM >= 0x075000)
#define RBL_CURL_HAS_PROTO_STR (LIBCURL_VERSION_NUM >= 0x075500)

#if RBL_CURL_HAS_PREREQ
/* Runs after the connection is established but before the request is sent, so
 * it sees the resolved peer IP and catches DNS names that point at private
 * space. Only installed when the connection is direct (with a proxy the peer
 * would be the proxy itself). */
static int prereq_cb(void *clientp, char *conn_primary_ip, char *conn_local_ip,
                     int primary_port, int local_port) {
    (void)conn_local_ip;
    (void)primary_port;
    (void)local_port;
    rbl_client *c = (rbl_client *)clientp;
    if (!c->cfg.allow_private_networks && rbl_ip_is_private(conn_primary_ip)) {
        rbl_logf("SSRF guard: refusing connection to private address %s",
                 conn_primary_ip);
        return CURL_PREREQFUNC_ABORT;
    }
    return CURL_PREREQFUNC_OK;
}
#endif

/* Resolve the URL's host and report whether it points at private space. Used
 * when the peer-IP hook cannot be: with an environment proxy we cannot tell
 * which connections bypass it (NO_PROXY, scheme-specific variables), and old
 * libcurl has no hook at all. */
static bool url_resolves_private(const char *url) {
    rbl_url u;
    if (!rbl_url_parse(url, &u)) return false;
    bool priv = rbl_host_resolves_private(u.host);
    rbl_url_free(&u);
    return priv;
}

static bool status_transient(rbl_status s, long code) {
    if (s == RBL_ERR_NETWORK || s == RBL_ERR_TIMEOUT) return true;
    if (code == 429 || code == 503 || code == 502 || code == 504) return true;
    return false;
}

static rbl_status map_curl_error(CURLcode rc, const fetch_ctx *ctx) {
    switch (rc) {
    case CURLE_OPERATION_TIMEDOUT:
        return RBL_ERR_TIMEOUT;
    case CURLE_TOO_MANY_REDIRECTS:
        return RBL_ERR_TOO_MANY_REDIRECTS;
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
        return RBL_ERR_BAD_URL;
    case CURLE_ABORTED_BY_CALLBACK:
        return RBL_ERR_BLOCKED;
    case CURLE_WRITE_ERROR:
        return ctx->too_large ? RBL_ERR_TOO_LARGE : RBL_ERR_NETWORK;
    default:
        return RBL_ERR_NETWORK;
    }
}

static rbl_status do_fetch(rbl_client *c, const char *url, const char *ua,
                           const char *al, const char *proxy, long timeout_ms,
                           rbl_http_result *out) {
    memset(out, 0, sizeof(*out));
    CURL *h = curl_easy_init();
    if (!h) return RBL_ERR_OOM;

    fetch_ctx ctx;
    rbl_buf_init(&ctx.body);
    ctx.content_type = NULL;
    ctx.max_bytes = c->cfg.max_response_bytes;
    ctx.too_large = false;

    struct curl_slist *hdrs = NULL;
    char al_hdr[512];
    snprintf(al_hdr, sizeof(al_hdr), "Accept-Language: %s", al);
    hdrs = curl_slist_append(
        hdrs, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
              "image/avif,image/webp,*/*;q=0.8");
    hdrs = curl_slist_append(hdrs, al_hdr);
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Dest: document");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Mode: navigate");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Site: none");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-User: ?1");
    hdrs = curl_slist_append(hdrs, "Upgrade-Insecure-Requests: 1");

    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_USERAGENT, ua);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, ""); /* advertise + decode all */
    /* Redirects are followed manually below so every hop can be vetted. */
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS,
                     timeout_ms < 15000 ? timeout_ms : 15000L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);

    /* Only http/https, for the request and anything it points at. */
#if RBL_CURL_HAS_PROTO_STR
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(h, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS,
                     (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

    /* SSRF: an explicit pool proxy resolves names itself, so only literal
     * hosts are checked. Direct connections get the peer-IP hook; with an
     * environment proxy (or no hook) the target is resolved per hop. */
    bool guard = !c->cfg.allow_private_networks;
    bool explicit_proxy = proxy && *proxy;
    bool dns_check = guard && !explicit_proxy && (c->proxied || !RBL_CURL_HAS_PREREQ);
#if RBL_CURL_HAS_PREREQ
    if (guard && !explicit_proxy && !c->proxied) {
        curl_easy_setopt(h, CURLOPT_PREREQFUNCTION, prereq_cb);
        curl_easy_setopt(h, CURLOPT_PREREQDATA, c);
    }
#endif
    if (c->share) curl_easy_setopt(h, CURLOPT_SHARE, c->share);
    /* An explicit pool proxy overrides the environment proxy. */
    if (proxy && *proxy) curl_easy_setopt(h, CURLOPT_PROXY, proxy);
    curl_easy_setopt(h, CURLOPT_COOKIEFILE,
                     c->cfg.cookie_file ? c->cfg.cookie_file : "");
    if (c->cfg.cookie_file) curl_easy_setopt(h, CURLOPT_COOKIEJAR, c->cfg.cookie_file);

    rbl_status st = RBL_OK;
    char *cur = rbl_strdup(url);
    long code = 0;
    int hops = 0;
    for (;;) {
        rbl_buf_reset(&ctx.body);
        free(ctx.content_type);
        ctx.content_type = NULL;
        ctx.too_large = false;

        if (dns_check && url_resolves_private(cur)) {
            rbl_logf("SSRF guard: %s resolves to a private address", cur);
            st = RBL_ERR_BLOCKED;
            break;
        }
        curl_easy_setopt(h, CURLOPT_URL, cur);
        CURLcode rc = curl_easy_perform(h);
        if (rc != CURLE_OK) {
            st = map_curl_error(rc, &ctx);
            rbl_logf("curl error on %s: %s", cur, curl_easy_strerror(rc));
            break;
        }
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        if (!is_redirect(code)) break;

        char *loc = NULL;
        curl_easy_getinfo(h, CURLINFO_REDIRECT_URL, &loc);
        if (!loc || !*loc) break; /* 3xx without Location: treat as final */
        if (++hops > c->cfg.max_redirects) {
            st = RBL_ERR_TOO_MANY_REDIRECTS;
            break;
        }
        rbl_url ru;
        if (!rbl_url_parse(loc, &ru)) { /* not http(s), or malformed */
            st = RBL_ERR_BAD_URL;
            break;
        }
        rbl_url_free(&ru);
        if (!c->cfg.allow_private_networks && rbl_url_host_is_private(loc)) {
            rbl_logf("SSRF guard: refusing redirect to private host %s", loc);
            st = RBL_ERR_BLOCKED;
            break;
        }
        char *next = rbl_strdup(loc); /* loc is owned by the handle */
        free(cur);
        cur = next;
        if (!cur) {
            st = RBL_ERR_OOM;
            break;
        }
    }

    out->http_code = code;
    out->final_url = cur; /* transfer ownership */
    out->content_type = ctx.content_type;
    ctx.content_type = NULL;

    if (st == RBL_OK) {
        if (out->content_type) {
            out->is_html = ct_is_html(out->content_type);
            out->is_text = ct_is_text(out->content_type);
        } else {
            sniff(ctx.body.data ? ctx.body.data : "", ctx.body.len, &out->is_text,
                  &out->is_html);
        }
        if (out->is_text) {
            /* Decode to UTF-8 using header charset, then <meta> sniffing. */
            char *cs = rbl_charset_from_content_type(out->content_type);
            if (!cs && out->is_html)
                cs = rbl_charset_from_html(ctx.body.data ? ctx.body.data : "",
                                           ctx.body.len);
            size_t ol = 0;
            out->body =
                rbl_to_utf8(ctx.body.data ? ctx.body.data : "", ctx.body.len, cs, &ol);
            out->body_len = ol;
            free(cs);
        } else {
            out->body = rbl_strdup(""); /* binary payloads are not decoded */
            out->body_len = 0;
        }
    }
    rbl_buf_free(&ctx.body);
    free(ctx.content_type);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);
    return st;
}

rbl_status rbl_http_get(rbl_client *c, const char *url, const char *ua, const char *al,
                        long timeout_ms, rbl_http_result *out) {
    if (!out) return RBL_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!c || !url) return RBL_ERR_INVALID_ARG;

    /* SSRF guard: reject loopback/private literal hosts up front. */
    if (!c->cfg.allow_private_networks && rbl_url_host_is_private(url)) {
        rbl_logf("SSRF guard: refusing private host in %s", url);
        return RBL_ERR_BLOCKED;
    }

    if (!ua) ua = c->ua;
    if (!al) al = c->accept_language;
    if (has_ctl(ua) || has_ctl(al)) return RBL_ERR_INVALID_ARG;
    if (timeout_ms <= 0) timeout_ms = c->cfg.default_timeout_ms;

    /* Host for sticky-per-host proxy selection. */
    rbl_url u;
    bool have_host = rbl_url_parse(url, &u);

    rbl_status st = RBL_ERR_NETWORK;
    long backoff = 500;
    for (int attempt = 0; attempt <= c->cfg.max_retries; attempt++) {
        if (attempt > 0) {
            rbl_logf("retry %d for %s (backoff %ldms)", attempt, url, backoff);
            rbl_sleep_ms(backoff);
            backoff *= 2;
        }
        char *proxy = rbl_client_pick_proxy(c, have_host ? u.host : NULL);
        st = do_fetch(c, url, ua, al, proxy, timeout_ms, out);
        free(proxy);
        if (st == RBL_OK && !status_transient(st, out->http_code)) goto done;
        if (st == RBL_OK) {
            /* Transient HTTP status (429/5xx): keep result for the last try. */
            if (attempt < c->cfg.max_retries)
                rbl_http_result_free(out);
            else
                goto done;
        } else {
            if (!status_transient(st, 0) || attempt == c->cfg.max_retries) goto done;
            rbl_http_result_free(out);
        }
    }
done:
    if (have_host) rbl_url_free(&u);
    return st;
}

void rbl_http_result_free(rbl_http_result *r) {
    if (!r) return;
    free(r->final_url);
    free(r->content_type);
    free(r->body);
    memset(r, 0, sizeof(*r));
}
