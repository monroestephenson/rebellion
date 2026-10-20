/* Minimal JSON helpers: string escaping (for output) and string-token
 * decoding / shallow key lookup (for parsing CDP replies). */
#ifndef REBELLION_JSON_H
#define REBELLION_JSON_H

#include "util.h"
#include <stddef.h>

/* Append a JSON-escaped string (without surrounding quotes) to b. */
void rbl_json_escape(rbl_buf *b, const char *s);
/* Append `"<escaped s>"` (with quotes) to b. */
void rbl_json_quote(rbl_buf *b, const char *s);

/* Decode a JSON string token. `p` must point at the opening quote. On success
 * returns a heap string (decoded) and, if end != NULL, sets *end to the char
 * just past the closing quote. Returns NULL on malformed input. */
char *rbl_json_decode_string(const char *p, const char **end);

/* Find the first occurrence of key `"name"` at/after `from` and return a
 * pointer to the first non-space character of its value (after the colon), or
 * NULL. This is a shallow scanner, adequate for the flat CDP replies we read,
 * not a general-purpose parser. */
const char *rbl_json_find_value(const char *json, const char *name);

#endif /* REBELLION_JSON_H */
