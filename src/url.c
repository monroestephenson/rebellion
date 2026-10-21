#include "url.h"
#include "util.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

static char *dup_part(CURLU *h, CURLUPart part, unsigned int flags) {
    char *tmp = NULL;
    if (curl_url_get(h, part, &tmp, flags) != CURLUE_OK || !tmp) return NULL;
    char *out = rbl_strdup(tmp);
    curl_free(tmp);
    return out;
}

bool rbl_url_parse(const char *in, rbl_url *out) {
    memset(out, 0, sizeof(*out));
    if (!in || !*in) return false;

    CURLU *h = curl_url();
    if (!h) return false;

    if (curl_url_set(h, CURLUPART_URL, in, 0) != CURLUE_OK) {
        curl_url_cleanup(h);
        return false;
    }

    out->scheme = dup_part(h, CURLUPART_SCHEME, 0);
    if (!out->scheme ||
        (!rbl_streq_ci(out->scheme, "http") && !rbl_streq_ci(out->scheme, "https"))) {
        curl_url_cleanup(h);
        rbl_url_free(out);
        return false;
    }
    rbl_str_tolower_inplace(out->scheme);

    out->host = dup_part(h, CURLUPART_HOST, 0);
    if (out->host) rbl_str_tolower_inplace(out->host);
    /* Default port filled in so comparisons are stable. */
    out->port = dup_part(h, CURLUPART_PORT, CURLU_DEFAULT_PORT);
    out->path = dup_part(h, CURLUPART_PATH, 0);
    out->query = dup_part(h, CURLUPART_QUERY, 0);
    out->full = dup_part(h, CURLUPART_URL, 0);

    curl_url_cleanup(h);

    if (!out->host || !out->full) {
        rbl_url_free(out);
        return false;
    }
    if (!out->path) out->path = rbl_strdup("/");
    return true;
}

/* Clean an href the way browsers do before resolving it (WHATWG URL): trim
 * surrounding whitespace/controls, drop tabs and newlines (hrefs wrapped
 * across lines), treat '\\' as '/' before any query/fragment, and
 * percent-encode the few characters curl would otherwise pass through raw. */
static char *clean_ref(const char *ref) {
    const char *s = ref, *e = ref + strlen(ref);
    while (s < e && (unsigned char)*s <= 0x20) s++;
    while (e > s && (unsigned char)e[-1] <= 0x20) e--;
    rbl_buf b;
    rbl_buf_init(&b);
    bool in_path = true; /* before '?' or '#' */
    for (const char *p = s; p < e; p++) {
        char ch = *p;
        if (ch == '\t' || ch == '\n' || ch == '\r') continue;
        if (ch == '?' || ch == '#') in_path = false;
        if (ch == '\\' && in_path)
            rbl_buf_append_ch(&b, '/');
        else if (ch == '"' || ch == '<' || ch == '>' || (ch == '`' && in_path))
            rbl_buf_appendf(&b, "%%%02X", (unsigned)(unsigned char)ch);
        else
            rbl_buf_append_ch(&b, ch);
    }
    return rbl_buf_steal(&b);
}

char *rbl_url_resolve(const char *base, const char *ref_in) {
    if (!ref_in) return NULL;
    char *ref = clean_ref(ref_in);
    if (!ref) return NULL;
    CURLU *h = curl_url();
    if (!h) {
        free(ref);
        return NULL;
    }

    char *result = NULL;
    /* Seed with the base, then apply the (possibly relative) reference. */
    if (base && *base) {
        if (curl_url_set(h, CURLUPART_URL, base, 0) != CURLUE_OK) goto done;
    }
    if (curl_url_set(h, CURLUPART_URL, ref, 0) != CURLUE_OK) goto done;

    /* Only accept http(s). */
    char *scheme = NULL;
    if (curl_url_get(h, CURLUPART_SCHEME, &scheme, 0) != CURLUE_OK || !scheme)
        goto done;
    bool ok = rbl_streq_ci(scheme, "http") || rbl_streq_ci(scheme, "https");
    curl_free(scheme);
    if (!ok) goto done;

    char *full = NULL;
    if (curl_url_get(h, CURLUPART_URL, &full, 0) == CURLUE_OK && full) {
        result = rbl_strdup(full);
        curl_free(full);
    }
done:
    curl_url_cleanup(h);
    free(ref);
    return result;
}

char *rbl_url_strip_fragment(const char *url) {
    if (!url) return NULL;
    const char *hash = strchr(url, '#');
    if (!hash) return rbl_strdup(url);
    return rbl_strndup(url, (size_t)(hash - url));
}

void rbl_url_free(rbl_url *u) {
    if (!u) return;
    free(u->full);
    free(u->scheme);
    free(u->host);
    free(u->port);
    free(u->path);
    free(u->query);
    memset(u, 0, sizeof(*u));
}

/* Return a pointer to the start of the last `labels` dot-separated labels. */
static const char *last_labels(const char *host, int labels) {
    const char *p = host + strlen(host);
    int seen = 0;
    while (p > host) {
        if (p[-1] == '.') {
            seen++;
            if (seen == labels) return p;
        }
        p--;
    }
    return host;
}

bool rbl_host_matches(const char *a, const char *b, bool allow_subdomains) {
    if (!a || !b) return false;
    if (rbl_streq_ci(a, b)) return true;
    if (!allow_subdomains) return false;
    /* Naive eTLD+1: compare the final two labels (handles foo.example.de vs
     * www.example.de). Not a public-suffix implementation, but good enough
     * to keep a crawl on one site without external data. */
    const char *ba = last_labels(a, 2);
    const char *bb = last_labels(b, 2);
    return rbl_streq_ci(ba, bb);
}
