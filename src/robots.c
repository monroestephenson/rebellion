#include "robots.h"
#include "internal.h"
#include "http.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>

/* Fetch (once) and cache robots.txt for the host. Returns the cached body or
 * NULL if there is none. */
static const char *get_robots(rbl_client *c, const rbl_url *u) {
    rbl_host_state *h = rbl_client_host(c, u->host);
    if (!h) return NULL;

    pthread_mutex_lock(&c->host_lock);
    bool done = h->robots_fetched;
    const char *cached = h->robots_body;
    pthread_mutex_unlock(&c->host_lock);
    if (done) return cached;

    /* Build robots URL: scheme://host[:port]/robots.txt */
    rbl_buf url; rbl_buf_init(&url);
    rbl_buf_appendf(&url, "%s://%s", u->scheme, u->host);
    if (u->port && *u->port) {
        bool is_default = (rbl_streq_ci(u->scheme, "http") && strcmp(u->port, "80") == 0) ||
                          (rbl_streq_ci(u->scheme, "https") && strcmp(u->port, "443") == 0);
        if (!is_default) rbl_buf_appendf(&url, ":%s", u->port);
    }
    rbl_buf_append_str(&url, "/robots.txt");

    char *body = NULL;
    rbl_http_result r;
    rbl_status st = rbl_http_get(c, rbl_buf_cstr(&url), NULL, NULL, 15000, &r);
    if (st == RBL_OK && r.http_code >= 200 && r.http_code < 300 && r.body)
        body = rbl_strdup(r.body);
    rbl_http_result_free(&r);
    rbl_buf_free(&url);

    pthread_mutex_lock(&c->host_lock);
    if (!h->robots_fetched) {
        h->robots_body = body;      /* may be NULL => allow all */
        h->robots_fetched = true;
        cached = h->robots_body;
    } else {
        free(body);
        cached = h->robots_body;
    }
    pthread_mutex_unlock(&c->host_lock);
    return cached;
}

/* A line iterator over robots.txt content. */
typedef struct { const char *p; } liter;

static bool next_line(liter *it, const char **key, size_t *klen,
                      const char **val, size_t *vlen) {
    while (*it->p) {
        const char *line = it->p;
        const char *eol = strpbrk(line, "\r\n");
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        it->p = eol ? (eol + 1) : (line + len);
        /* strip comments */
        const char *hash = memchr(line, '#', len);
        if (hash) len = (size_t)(hash - line);
        /* split key:value */
        const char *colon = memchr(line, ':', len);
        if (!colon) continue;
        const char *k = line; size_t kl = (size_t)(colon - line);
        const char *v = colon + 1; size_t vl = len - kl - 1;
        while (kl && isspace((unsigned char)k[0])) { k++; kl--; }
        while (kl && isspace((unsigned char)k[kl-1])) kl--;
        while (vl && isspace((unsigned char)v[0])) { v++; vl--; }
        while (vl && isspace((unsigned char)v[vl-1])) vl--;
        if (kl == 0) continue;
        *key = k; *klen = kl; *val = v; *vlen = vl;
        return true;
    }
    return false;
}

static bool key_is(const char *k, size_t kl, const char *name) {
    size_t n = strlen(name);
    return kl == n && strncasecmp(k, name, n) == 0;
}

/* Does robots pattern `pat` match a prefix of `path`? Supports '*' wildcards
 * and a trailing '$' end-anchor, per the de-facto robots spec. A disallow
 * rule matches when the pattern matches the start of the path. */
static bool robots_match(const char *pat, size_t patlen, const char *path) {
    size_t slen = strlen(path);
    size_t pi = 0, si = 0;
    size_t star = (size_t)-1, smark = 0;

    while (si < slen) {
        if (pi < patlen && pat[pi] == '$' && pi + 1 == patlen) {
            return si == slen; /* false here, since si < slen */
        }
        if (pi < patlen && pat[pi] == '*') {
            star = pi++; smark = si;
        } else if (pi < patlen && pat[pi] == path[si]) {
            pi++; si++;
        } else if (star != (size_t)-1) {
            pi = star + 1; si = ++smark;
        } else {
            return false;
        }
        if (pi == patlen) return true; /* pattern consumed => prefix match */
    }
    /* Path exhausted: swallow any trailing '*', then honor an optional '$'. */
    while (pi < patlen && pat[pi] == '*') pi++;
    if (pi < patlen && pat[pi] == '$' && pi + 1 == patlen) return true;
    return pi == patlen;
}

typedef struct {
    bool have_match;       /* did any group's UA token match us? */
    /* best rule found */
    size_t best_len;
    bool   best_allow;
    long   crawl_delay;
} robots_eval;

/* Evaluate robots content against path for ua_token (and "*"). */
static void eval_robots(const char *content, const char *path,
                        const char *ua_token, robots_eval *out) {
    memset(out, 0, sizeof(*out));
    out->best_allow = true;
    out->best_len = 0;
    out->crawl_delay = -1;
    if (!content) return;

    liter it; it.p = content;
    const char *k, *v; size_t kl, vl;

    /* Two passes conceptually: collect rules for groups that apply to us.
     * A group applies if any of its User-agent lines is "*" or matches our
     * token (case-insensitive substring). Specific-token groups override the
     * "*" group per spec; we approximate by preferring specific matches. */
    bool in_group = false;        /* currently inside an applicable group */
    bool group_specific = false;  /* applicable via our token, not "*"    */
    bool specific_seen = false;   /* any specific group matched at all     */
    bool reading_agents = false;  /* consecutive UA lines form one group   */

    /* First scan: determine if a token-specific group exists. */
    {
        liter it2; it2.p = content;
        const char *kk, *vv; size_t kkl, vvl;
        bool agents = false;
        while (next_line(&it2, &kk, &kkl, &vv, &vvl)) {
            if (key_is(kk, kkl, "user-agent")) {
                char *ua = rbl_strndup(vv, vvl);
                rbl_str_tolower_inplace(ua);
                if (ua_token && *ua_token && strstr(ua, ua_token)) specific_seen = true;
                free(ua);
                agents = true;
            } else {
                agents = false;
            }
            (void)agents;
        }
    }

    while (next_line(&it, &k, &kl, &v, &vl)) {
        if (key_is(k, kl, "user-agent")) {
            if (!reading_agents) {
                /* New group begins: reset applicability. */
                in_group = false;
                group_specific = false;
                reading_agents = true;
            }
            char *ua = rbl_strndup(v, vl);
            rbl_str_tolower_inplace(ua);
            if (strcmp(ua, "*") == 0) {
                if (!specific_seen) in_group = true;  /* use * only if no specific */
            }
            if (ua_token && *ua_token && strstr(ua, ua_token)) {
                in_group = true; group_specific = true;
            }
            free(ua);
            continue;
        }
        reading_agents = false;
        if (!in_group) continue;
        /* Only let the specific group win when one exists. */
        if (specific_seen && !group_specific) continue;

        if (key_is(k, kl, "disallow") || key_is(k, kl, "allow")) {
            bool allow = key_is(k, kl, "allow");
            if (vl == 0) {
                /* Empty Disallow = allow all; empty Allow = no-op. */
                continue;
            }
            if (robots_match(v, vl, path)) {
                if (vl >= out->best_len) {
                    out->best_len = vl;
                    out->best_allow = allow;
                    out->have_match = true;
                }
            }
        } else if (key_is(k, kl, "crawl-delay")) {
            char *s = rbl_strndup(v, vl);
            double d = atof(s);
            if (d > 0) out->crawl_delay = (long)(d * 1000.0);
            free(s);
        }
    }
}

bool rbl_robots_path_match(const char *pattern, const char *path) {
    if (!pattern || !path) return false;
    return robots_match(pattern, strlen(pattern), path);
}

bool rbl_robots_eval_allowed(const char *content, const char *path,
                             const char *ua_token) {
    robots_eval ev;
    eval_robots(content, path ? path : "/", ua_token, &ev);
    return ev.best_allow;
}

bool rbl_robots_allowed(rbl_client *c, const rbl_url *u, const char *ua_token) {
    const char *content = get_robots(c, u);
    if (!content) return true;
    const char *path = (u->path && *u->path) ? u->path : "/";
    robots_eval ev;
    eval_robots(content, path, ua_token, &ev);
    return ev.best_allow;
}

long rbl_robots_crawl_delay(rbl_client *c, const rbl_url *u, const char *ua_token) {
    const char *content = get_robots(c, u);
    if (!content) return -1;
    const char *path = (u->path && *u->path) ? u->path : "/";
    robots_eval ev;
    eval_robots(content, path, ua_token, &ev);
    return ev.crawl_delay;
}
