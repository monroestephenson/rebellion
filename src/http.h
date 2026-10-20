/* HTTP transport built on libcurl, with charset normalization to UTF-8. */
#ifndef REBELLION_HTTP_H
#define REBELLION_HTTP_H

#include "rebellion/rebellion.h"
#include <stddef.h>

typedef struct {
    long http_code;
    char *final_url;    /* after redirects (owned)                */
    char *content_type; /* raw Content-Type header (owned) or NULL */
    char *body;         /* UTF-8, NUL-terminated (owned); "" for
                         * non-text responses                      */
    size_t body_len;
    bool is_html; /* HTML/XML (by Content-Type or sniffing)  */
    bool is_text; /* any textual type (HTML, text, JSON...)  */
} rbl_http_result;

/* GET a URL. user_agent / accept_language may be NULL to use client defaults.
 * timeout_ms <= 0 uses the client default. Body is decoded to UTF-8. Retries
 * transient transport failures per the client config. */
rbl_status rbl_http_get(rbl_client *c, const char *url, const char *user_agent,
                        const char *accept_language, long timeout_ms,
                        rbl_http_result *out);

void rbl_http_result_free(rbl_http_result *r);

#endif /* REBELLION_HTTP_H */
