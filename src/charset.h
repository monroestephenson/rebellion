/* Character-set detection and conversion to UTF-8.
 *
 * German sites still commonly serve ISO-8859-1 / ISO-8859-15 / Windows-1252.
 * Getting this right is what makes "ä ö ü ß €" come out intact. */
#ifndef REBELLION_CHARSET_H
#define REBELLION_CHARSET_H

#include <stddef.h>

/* Extract a charset token from a Content-Type header value, e.g.
 * "text/html; charset=ISO-8859-1". Returns a heap string (lowercased,
 * caller frees) or NULL if absent. */
char *rbl_charset_from_content_type(const char *content_type);

/* Sniff a charset from the first bytes of an HTML document: a BOM, a
 * <meta charset=...>, or a <meta http-equiv="Content-Type" ...>. Returns a
 * heap string (lowercased) or NULL. Only scans the first few KiB. */
char *rbl_charset_from_html(const char *data, size_t len);

/* Convert `in`/`in_len` from `from_charset` to UTF-8. If from_charset is NULL
 * or already UTF-8, the input is validated/sanitized and copied. On success
 * returns a NUL-terminated heap string and sets *out_len (excluding the NUL).
 * Never returns NULL for valid input; falls back to a best-effort latin1
 * mapping if iconv does not know the source encoding. */
char *rbl_to_utf8(const char *in, size_t in_len, const char *from_charset,
                  size_t *out_len);

#endif /* REBELLION_CHARSET_H */
