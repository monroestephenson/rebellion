/* HTML entity decoding into UTF-8. */
#ifndef REBELLION_ENTITIES_H
#define REBELLION_ENTITIES_H

#include "util.h"
#include <stddef.h>

/* Append a Unicode code point to buf as UTF-8. */
void rbl_utf8_append(rbl_buf *b, unsigned long cp);

/* Decode a run of text that may contain entities, appending UTF-8 to `out`.
 * Handles &name; &#123; &#xAB; and tolerates missing semicolons for the
 * common named entities (as browsers do). */
void rbl_decode_entities(rbl_buf *out, const char *s, size_t len);

/* Convenience: decode a NUL-terminated string into a fresh heap string. */
char *rbl_decode_entities_str(const char *s);

/* Same, but for a counted (non-NUL-terminated) slice. */
char *rbl_decode_entities_str_n(const char *s, size_t len);

#endif /* REBELLION_ENTITIES_H */
