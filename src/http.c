#include "http.h"
#include "internal.h"
#include "charset.h"
#include "util.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    rbl_buf body;
    char   *content_type; /* captured from headers */
} fetch_ctx;

static size_t write_body(char *ptr, size_t size, size_t nmemb, void *ud) {
    fetch_ctx *ctx = (fetch_ctx *)ud;
    size_t n = size * nmemb;
    /* Guard against pathologically large responses (256 MiB). */
    if (ctx->body.len + n > (size_t)256 * 1024 * 1024) return 0;
    if (!rbl_buf_append(&ctx->body, ptr, n)) return 0;
    return n;
}

static size_t header_cb(char *buffer, size_t size, size_t nitems, void *ud) {
    fetch_ctx *ctx = (fetch_ctx *)ud;
    size_t n = size * nitems;
    if (n > 14 && strncasecmp(buffer, "Content-Type:", 13) == 0) {
        const char *v = buffer + 13;
        size_t vl = n - 13;
        while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
        while (vl && (v[vl-1] == '\r' || v[vl-1] == '\n' ||
                      v[vl-1] == ' '  || v[vl-1] == '\t')) vl--;
        free(ctx->content_type);
        ctx->content_type = rbl_strndup(v, vl);
    }
    return n;
}

static bool ct_is_html(const char *ct) {
    if (!ct) return false;
    return strcasestr(ct, "text/html") || strcasestr(ct, "application/xhtml") ||
           strcasestr(ct, "text/xml")  || strcasestr(ct, "application/xml")  ||
           strcasestr(ct, "application/rss") || strcasestr(ct, "+xml");
}

static bool status_transient(rbl_status s, long code) {
    if (s == RBL_ERR_NETWORK || s == RBL_ERR_TIMEOUT) return true;
    if (code == 429 || code == 503 || code == 502 || code == 504) return true;
    return false;
}

static rbl_status do_fetch(rbl_client *c, const char *url,
                           const char *ua, const char *al,
                           long timeout_ms, rbl_http_result *out) {
    CURL *h = curl_easy_init();
    if (!h) return RBL_ERR_OOM;

    fetch_ctx ctx;
    rbl_buf_init(&ctx.body);
    ctx.content_type = NULL;

    struct curl_slist *hdrs = NULL;
    char al_hdr[256];
    snprintf(al_hdr, sizeof(al_hdr), "Accept-Language: %s", al);
    hdrs = curl_slist_append(hdrs,
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
        "image/avif,image/webp,*/*;q=0.8");
    hdrs = curl_slist_append(hdrs, al_hdr);
    hdrs = curl_slist_append(hdrs, "Accept-Encoding: gzip, deflate, br, zstd");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Dest: document");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Mode: navigate");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Site: none");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-User: ?1");
    hdrs = curl_slist_append(hdrs, "Upgrade-Insecure-Requests: 1");

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_USERAGENT, ua);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, ""); /* let curl advertise+decode */
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, (long)c->cfg.max_redirects);
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
    /* Proxy environment (HTTPS_PROXY) is honored by libcurl automatically. */
    if (c->share) curl_easy_setopt(h, CURLOPT_SHARE, c->share);
    /* In-memory cookie engine (or a file if configured). */
    curl_easy_setopt(h, CURLOPT_COOKIEFILE,
                     c->cfg.cookie_file ? c->cfg.cookie_file : "");
    if (c->cfg.cookie_file)
        curl_easy_setopt(h, CURLOPT_COOKIEJAR, c->cfg.cookie_file);

    CURLcode rc = curl_easy_perform(h);

    rbl_status st = RBL_OK;
    if (rc != CURLE_OK) {
        switch (rc) {
            case CURLE_OPERATION_TIMEDOUT: st = RBL_ERR_TIMEOUT; break;
            case CURLE_TOO_MANY_REDIRECTS: st = RBL_ERR_TOO_MANY_REDIRECTS; break;
            case CURLE_URL_MALFORMAT:      st = RBL_ERR_BAD_URL; break;
            case CURLE_UNSUPPORTED_PROTOCOL: st = RBL_ERR_BAD_URL; break;
            default:                        st = RBL_ERR_NETWORK; break;
        }
        rbl_logf("curl error on %s: %s", url, curl_easy_strerror(rc));
    }

    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    char *eff = NULL;
    curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff);

    memset(out, 0, sizeof(*out));
    out->http_code = code;
    out->final_url = rbl_strdup(eff ? eff : url);
    out->content_type = ctx.content_type; /* transfer ownership */
    out->is_html = ct_is_html(ctx.content_type);

    if (st == RBL_OK) {
        /* Decode to UTF-8 using header charset, then HTML meta sniffing. */
        char *cs = rbl_charset_from_content_type(ctx.content_type);
        if (!cs && out->is_html)
            cs = rbl_charset_from_html(ctx.body.data ? ctx.body.data : "",
                                       ctx.body.len);
        size_t ol = 0;
        out->body = rbl_to_utf8(ctx.body.data ? ctx.body.data : "",
                                ctx.body.len, cs, &ol);
        out->body_len = ol;
        free(cs);
    }
    rbl_buf_free(&ctx.body);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);

    if (st != RBL_OK) return st;
    return RBL_OK;
}

rbl_status rbl_http_get(rbl_client *c, const char *url,
                        const char *ua, const char *al,
                        long timeout_ms, rbl_http_result *out) {
    if (!c || !url || !out) return RBL_ERR_INVALID_ARG;
    if (!ua) ua = c->ua;
    if (!al) al = c->accept_language;
    if (timeout_ms <= 0) timeout_ms = c->cfg.default_timeout_ms;

    rbl_status st = RBL_ERR_NETWORK;
    long backoff = 500;
    for (int attempt = 0; attempt <= c->cfg.max_retries; attempt++) {
        if (attempt > 0) {
            rbl_logf("retry %d for %s (backoff %ldms)", attempt, url, backoff);
            rbl_sleep_ms(backoff);
            backoff *= 2;
        }
        st = do_fetch(c, url, ua, al, timeout_ms, out);
        if (st == RBL_OK && !status_transient(st, out->http_code))
            return RBL_OK;
        if (st == RBL_OK) {
            /* Transient HTTP status (429/5xx): keep result for the last try. */
            if (attempt < c->cfg.max_retries) rbl_http_result_free(out);
            else return RBL_OK;
        } else {
            if (!status_transient(st, 0) || attempt == c->cfg.max_retries)
                return st;
            rbl_http_result_free(out);
        }
    }
    return st;
}

void rbl_http_result_free(rbl_http_result *r) {
    if (!r) return;
    free(r->final_url);
    free(r->content_type);
    free(r->body);
    memset(r, 0, sizeof(*r));
}
