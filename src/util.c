#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

/* ---- buffer --------------------------------------------------------- */

void rbl_buf_init(rbl_buf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void rbl_buf_free(rbl_buf *b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void rbl_buf_reset(rbl_buf *b) {
    b->len = 0;
}

bool rbl_buf_reserve(rbl_buf *b, size_t extra) {
    /* Keep one spare byte so rbl_buf_cstr can always NUL-terminate. */
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    char *p = (char *)realloc(b->data, cap);
    if (!p) return false;
    b->data = p;
    b->cap = cap;
    return true;
}

bool rbl_buf_append(rbl_buf *b, const void *p, size_t n) {
    if (n == 0) return true;
    if (!rbl_buf_reserve(b, n)) return false;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return true;
}

bool rbl_buf_append_str(rbl_buf *b, const char *s) {
    return s ? rbl_buf_append(b, s, strlen(s)) : true;
}

bool rbl_buf_append_ch(rbl_buf *b, char c) {
    if (!rbl_buf_reserve(b, 1)) return false;
    b->data[b->len++] = c;
    return true;
}

bool rbl_buf_appendf(rbl_buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return false;
    }
    if (!rbl_buf_reserve(b, (size_t)n + 1)) {
        va_end(ap2);
        return false;
    }
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
    return true;
}

char *rbl_buf_cstr(rbl_buf *b) {
    if (!rbl_buf_reserve(b, 0)) return NULL;
    b->data[b->len] = '\0';
    return b->data;
}

char *rbl_buf_steal(rbl_buf *b) {
    if (!rbl_buf_cstr(b)) return NULL;
    char *p = b->data;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return p;
}

/* ---- strings -------------------------------------------------------- */

char *rbl_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *rbl_strndup(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    if (n && s) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

bool rbl_streq_ci(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

bool rbl_strcaseprefix(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
        s++;
        prefix++;
    }
    return true;
}

void rbl_str_tolower_inplace(char *s) {
    if (!s) return;
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

char *rbl_strdup_lower(const char *s) {
    char *p = rbl_strdup(s);
    rbl_str_tolower_inplace(p);
    return p;
}

char *rbl_trim(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

void rbl_collapse_ws(char *s) {
    if (!s) return;
    char *w = s;
    bool in_ws = false;
    bool started = false;
    for (char *r = s; *r; r++) {
        if (isspace((unsigned char)*r)) {
            in_ws = true;
        } else {
            if (in_ws && started) *w++ = ' ';
            *w++ = *r;
            in_ws = false;
            started = true;
        }
    }
    *w = '\0';
}

/* ---- string set ----------------------------------------------------- */

static unsigned long fnv1a(const char *s) {
    unsigned long h = 1469598103934665603UL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211UL;
    }
    return h;
}

void rbl_strset_init(rbl_strset *s) {
    s->nbuckets = 64;
    s->count = 0;
    s->buckets = (rbl_strset_node **)calloc(s->nbuckets, sizeof(*s->buckets));
    if (!s->buckets) s->nbuckets = 0;
}

void rbl_strset_free(rbl_strset *s) {
    if (!s || !s->buckets) return;
    for (size_t i = 0; i < s->nbuckets; i++) {
        rbl_strset_node *n = s->buckets[i];
        while (n) {
            rbl_strset_node *nx = n->next;
            free(n->key);
            free(n);
            n = nx;
        }
    }
    free(s->buckets);
    s->buckets = NULL;
    s->nbuckets = 0;
    s->count = 0;
}

static void strset_grow(rbl_strset *s) {
    size_t nb = s->nbuckets * 2;
    rbl_strset_node **nbk = (rbl_strset_node **)calloc(nb, sizeof(*nbk));
    if (!nbk) return;
    for (size_t i = 0; i < s->nbuckets; i++) {
        rbl_strset_node *n = s->buckets[i];
        while (n) {
            rbl_strset_node *nx = n->next;
            size_t b = n->hash % nb;
            n->next = nbk[b];
            nbk[b] = n;
            n = nx;
        }
    }
    free(s->buckets);
    s->buckets = nbk;
    s->nbuckets = nb;
}

bool rbl_strset_contains(const rbl_strset *s, const char *key) {
    if (!s || !s->buckets || !key) return false;
    unsigned long h = fnv1a(key);
    for (rbl_strset_node *n = s->buckets[h % s->nbuckets]; n; n = n->next)
        if (n->hash == h && strcmp(n->key, key) == 0) return true;
    return false;
}

bool rbl_strset_add(rbl_strset *s, const char *key) {
    if (!s || !key) return false;
    if (!s->buckets) {
        rbl_strset_init(s);
        if (!s->buckets) return false;
    }
    unsigned long h = fnv1a(key);
    size_t b = h % s->nbuckets;
    for (rbl_strset_node *n = s->buckets[b]; n; n = n->next)
        if (n->hash == h && strcmp(n->key, key) == 0) return false;
    if (s->count + 1 > s->nbuckets * 3 / 2) {
        strset_grow(s);
        b = h % s->nbuckets;
    }
    rbl_strset_node *n = (rbl_strset_node *)malloc(sizeof(*n));
    if (!n) return false;
    n->key = rbl_strdup(key);
    if (!n->key) {
        free(n);
        return false;
    }
    n->hash = h;
    n->next = s->buckets[b];
    s->buckets[b] = n;
    s->count++;
    return true;
}

/* ---- logging -------------------------------------------------------- */

static bool g_verbose = false;
void rbl_log_set_verbose(bool v) {
    g_verbose = v;
}

void rbl_logf(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[rebellion] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---- time ----------------------------------------------------------- */

long long rbl_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void rbl_sleep_ms(long ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

unsigned char *rbl_base64_decode(const char *in, size_t in_len, size_t *out_len) {
    signed char dec[256];
    for (int i = 0; i < 256; i++) dec[i] = -1;
    const char *abc =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) dec[(unsigned char)abc[i]] = (signed char)i;

    unsigned char *out = (unsigned char *)malloc(in_len / 4 * 3 + 4);
    if (!out) return NULL;
    size_t o = 0;
    int val = 0, bits = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=') break;
        signed char d = dec[c];
        if (d < 0) continue; /* skip whitespace/newlines */
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)((val >> bits) & 0xFF);
        }
    }
    if (out_len) *out_len = o;
    return out;
}

char *rbl_base64_encode(const unsigned char *in, size_t in_len) {
    static const char abc[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t olen = 4 * ((in_len + 2) / 3);
    char *out = (char *)malloc(olen + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < in_len;) {
        unsigned a = i < in_len ? in[i++] : 0;
        unsigned b = i < in_len ? in[i++] : 0;
        unsigned c = i < in_len ? in[i++] : 0;
        unsigned triple = (a << 16) | (b << 8) | c;
        out[o++] = abc[(triple >> 18) & 0x3F];
        out[o++] = abc[(triple >> 12) & 0x3F];
        out[o++] = abc[(triple >> 6) & 0x3F];
        out[o++] = abc[triple & 0x3F];
    }
    /* padding */
    size_t mod = in_len % 3;
    if (mod == 1) {
        out[olen - 1] = '=';
        out[olen - 2] = '=';
    } else if (mod == 2) {
        out[olen - 1] = '=';
    }
    out[olen] = '\0';
    return out;
}
