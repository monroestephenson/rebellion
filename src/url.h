/* URL parsing / resolution helpers, backed by libcurl's URL API. */
#ifndef REBELLION_URL_H
#define REBELLION_URL_H

#include <stdbool.h>

typedef struct {
    char *full;     /* normalized absolute URL                */
    char *scheme;   /* "http" / "https"                       */
    char *host;     /* lowercased host, no port               */
    char *port;     /* explicit or scheme-default              */
    char *path;     /* path (always starts with '/')          */
    char *query;    /* without leading '?', or NULL           */
} rbl_url;

/* Parse an absolute URL. Returns false if invalid or not http(s). */
bool rbl_url_parse(const char *in, rbl_url *out);

/* Resolve `ref` (absolute or relative) against `base`. On success returns a
 * heap string (caller frees); NULL on failure or non-http(s) scheme. */
char *rbl_url_resolve(const char *base, const char *ref);

/* Strip the fragment (#...) from a URL, returning a heap copy. */
char *rbl_url_strip_fragment(const char *url);

void rbl_url_free(rbl_url *u);

/* True if host `a` equals host `b`, optionally treating subdomains of the
 * registrable-ish base as equal (naive: compares the last two labels). */
bool rbl_host_matches(const char *a, const char *b, bool allow_subdomains);

#endif /* REBELLION_URL_H */
