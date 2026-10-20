/* Internal utilities: growable byte buffer, string helpers, logging. */
#ifndef REBELLION_UTIL_H
#define REBELLION_UTIL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

/* ---- growable buffer (also used as a string builder) ---------------- */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} rbl_buf;

void  rbl_buf_init(rbl_buf *b);
void  rbl_buf_free(rbl_buf *b);
void  rbl_buf_reset(rbl_buf *b);           /* len=0, keep capacity */
bool  rbl_buf_reserve(rbl_buf *b, size_t extra);
bool  rbl_buf_append(rbl_buf *b, const void *p, size_t n);
bool  rbl_buf_append_str(rbl_buf *b, const char *s);
bool  rbl_buf_append_ch(rbl_buf *b, char c);
bool  rbl_buf_appendf(rbl_buf *b, const char *fmt, ...);
/* Ensure a trailing NUL without counting it in len. Returns b->data. */
char *rbl_buf_cstr(rbl_buf *b);
/* Detach the buffer into a heap C string (NUL-terminated); resets b. */
char *rbl_buf_steal(rbl_buf *b);

/* ---- string helpers ------------------------------------------------- */
char  *rbl_strdup(const char *s);
char  *rbl_strndup(const char *s, size_t n);
bool   rbl_streq_ci(const char *a, const char *b);
bool   rbl_strcaseprefix(const char *s, const char *prefix); /* ci prefix */
char  *rbl_strdup_lower(const char *s);
void   rbl_str_tolower_inplace(char *s);
/* Trim ASCII whitespace; returns pointer into s and may write a NUL. */
char  *rbl_trim(char *s);
/* Collapse runs of ASCII whitespace into single spaces, in place. */
void   rbl_collapse_ws(char *s);

/* ---- string set (hash set of owned C strings) ----------------------- */
typedef struct rbl_strset_node {
    char *key;
    unsigned long hash;
    struct rbl_strset_node *next;
} rbl_strset_node;

typedef struct {
    rbl_strset_node **buckets;
    size_t nbuckets;
    size_t count;
} rbl_strset;

void rbl_strset_init(rbl_strset *s);
void rbl_strset_free(rbl_strset *s);
/* Returns true if newly added, false if already present (or on OOM). */
bool rbl_strset_add(rbl_strset *s, const char *key);
bool rbl_strset_contains(const rbl_strset *s, const char *key);

/* ---- logging -------------------------------------------------------- */
void rbl_log_set_verbose(bool v);
void rbl_logf(const char *fmt, ...);

/* ---- misc ----------------------------------------------------------- */
/* Monotonic-ish milliseconds since an arbitrary epoch. */
long long rbl_now_ms(void);
void rbl_sleep_ms(long ms);

/* Decode base64 into a heap buffer. Returns bytes and sets *out_len, or NULL
 * on malformed input. Ignores embedded whitespace. */
unsigned char *rbl_base64_decode(const char *in, size_t in_len, size_t *out_len);

/* Encode bytes as base64 (NUL-terminated heap string). */
char *rbl_base64_encode(const unsigned char *in, size_t in_len);

#endif /* REBELLION_UTIL_H */
